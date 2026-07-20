#include "ThreadProcessImage.hpp"
#include "JPEG.hpp"
#include "utility_TimeRecorder.hpp"
#include "utility_compass.hpp"
#include "utility_directory.hpp"
#include "utility_string.hpp"
#include "utility_time.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <numeric> // std::iota
#ifdef USE_KEBBI
#include "Kebbi/RobotCommand.pb.h"
#elif USE_ZENBO
#include "Zenbo/RobotCommand.pb.h"
#endif
#include "utility_directory.hpp"

#include "RobotStatus.hpp"
#include "YoloDetector.hpp"

// Compiled protobuf headers for MediaPipe types used
// The two files are at
// /home/chihyuan/mediapipe/bazel-bin/mediapipe/framework/formats They are
// included by target_include_directories(MP_FORMATS PUBLIC
// "${MEDIAPIPE_DIR}/bazel-bin") in the CMakeLists.txt However, they seem
// uncessary here because I move them to the GetLandmarks.hpp #include
// "mediapipe/framework/formats/landmark.pb.h" #include
// "mediapipe/framework/formats/image_format.pb.h"

#include <google/protobuf/text_format.h>
#include "mediapipe/framework/formats/detection.pb.h"

#include "GetLandmarks.hpp"
#include "LandmarkToRobotAction.hpp"

// xt::argmax
#include "xtensor/containers/xarray.hpp"
#include "xtensor/core/xmath.hpp"
#include "xtensor/io/xio.hpp"
#include "xtensor/misc/xsort.hpp"

RobotStatus robot_status;

int is_dancing = 0;

static std::vector<mediapipe::Detection> GetDetectionsFromPacket(const std::shared_ptr<mediapipe::LibMP>& libmp,
                                                                     const char* stream_name) {
    std::vector<mediapipe::Detection> detections;

    while (libmp->GetOutputQueueSize(stream_name) > 0) {
        const void* packet = libmp->GetOutputPacket(stream_name);
        if (packet == nullptr) {
            continue;
        }
        if (mediapipe::LibMP::PacketIsEmpty(packet)) {
            mediapipe::LibMP::DeletePacket(packet);
            continue;
        }

        // Your LibMP wrapper implements only protobuf-based accessors for packets.
        // For object_detection graphs, output_detections is encoded as a protobuf
        // detection list, which can be accessed via LibMP::GetPacketProtoMsgVecSize
        // + GetPacketProtoMsgAt.
        //
        // NOTE: We intentionally avoid any LibMP typed Packet::Get<vector<Detection>>()
        // because LibMPImpl doesn't expose it here.

        // `output_detections` is a native vector<Detection>, not a protobuf
        // MessageLite.  Calling GetPacketProtoMsg() on it is fatal inside
        // MediaPipe, so query vector access directly.
        size_t vecSize = mediapipe::LibMP::GetPacketProtoMsgVecSize(packet);
        if (vecSize == 0) {
            mediapipe::LibMP::DeletePacket(packet);
            break;
        }

        for (size_t i = 0; i < vecSize; ++i) {
            const void* proto_i = mediapipe::LibMP::GetPacketProtoMsgAt(packet, static_cast<unsigned int>(i));
            if (proto_i == nullptr) continue;
            const auto* det_msg = reinterpret_cast<const mediapipe::Detection*>(proto_i);
            if (det_msg) detections.push_back(*det_msg);
        }

        mediapipe::LibMP::DeletePacket(packet);
        break; // we processed one packet
    }

    return detections;
}


static std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static std::string DetectionLabel(const mediapipe::Detection& det) {
    if (det.display_name_size() > 0) {
        return det.display_name(0);
    }
    if (det.label_size() > 0) {
        return det.label(0);
    }
    if (det.label_id_size() > 0) {
        return "id=" + std::to_string(det.label_id(0));
    }
    return "";
}

static bool LooksLikeHandheldPhoneBox(const cv::Rect& rect, const cv::Mat& frame) {
    if (frame.empty() || rect.width <= 0 || rect.height <= 0) {
        return false;
    }

    const float image_area = static_cast<float>(frame.cols * frame.rows);
    const float box_area = static_cast<float>(rect.area());
    const float area_ratio = image_area > 0.0f ? box_area / image_area : 0.0f;
    const float aspect = static_cast<float>(rect.height) / static_cast<float>(rect.width);

    // Phones are usually tall rectangles. A looser ratio caused vertical mice
    // and other hand-held objects to be renamed as phones.
    return aspect >= 1.75f && aspect <= 4.5f && area_ratio >= 0.015f && area_ratio <= 0.35f;
}

static bool ShouldDrawDetection(const mediapipe::Detection& det, const cv::Rect& rect, const cv::Mat& frame,
                                std::string& label_text) {
    if (det.score_size() == 0) {
        return false;
    }

    const float score = det.score(0);
    label_text = DetectionLabel(det);
    const std::string label = ToLowerAscii(label_text);

    // Use per-label thresholds. Small phones in hands often score lower than
    // large people/faces, while bicycle false positives in indoor scenes need
    // to stay very strict.
    float minimum_score = 0.55f;
    const bool likely_handheld_phone =
        (label == "microwave" || label == "cell phone") && LooksLikeHandheldPhoneBox(rect, frame);

    if (label == "person") {
        minimum_score = 0.50f;
    } else if (label == "cell phone" || label == "remote" || label == "mouse" || label == "book" ||
               label == "cup" || label == "bottle" || likely_handheld_phone) {
        minimum_score = 0.20f;
    }

    if (label == "bicycle") {
        minimum_score = 0.90f;
        const float image_area = static_cast<float>(frame.cols * frame.rows);
        const float box_area = static_cast<float>(rect.area());
        if (image_area <= 0.0f || box_area / image_area < 0.08f) {
            return false;
        }
    }

    if (score < minimum_score) {
        return false;
    }

    if (label == "person") {
        label_text = "human/person";
    } else if (likely_handheld_phone) {
        label_text = "mobile phone";
    } else if (label_text.empty()) {
        label_text = "object";
    }

    char score_buf[64];
    snprintf(score_buf, sizeof(score_buf), " %.2f", score);
    label_text += score_buf;
    return true;
}

static void DrawDetections(cv::Mat& frame, const std::vector<mediapipe::Detection>& detections) {
    for (const auto& det : detections) {
        if (!det.has_location_data()) {
            continue;
        }
        const mediapipe::LocationData& loc = det.location_data();
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        if (loc.has_relative_bounding_box()) {
            const mediapipe::LocationData::RelativeBoundingBox& rbox = loc.relative_bounding_box();
            x = rbox.xmin();
            y = rbox.ymin();
            w = rbox.width();
            h = rbox.height();
        } else if (loc.has_bounding_box()) {
            const mediapipe::LocationData::BoundingBox& box = loc.bounding_box();
            x = static_cast<float>(box.xmin()) / static_cast<float>(frame.cols);
            y = static_cast<float>(box.ymin()) / static_cast<float>(frame.rows);
            w = static_cast<float>(box.width()) / static_cast<float>(frame.cols);
            h = static_cast<float>(box.height()) / static_cast<float>(frame.rows);
        } else {
            continue;
        }

        int left = static_cast<int>(x * frame.cols);
        int top = static_cast<int>(y * frame.rows);
        int width = static_cast<int>(w * frame.cols);
        int height = static_cast<int>(h * frame.rows);
        left = std::max(0, std::min(left, frame.cols - 1));
        top = std::max(0, std::min(top, frame.rows - 1));
        width = std::max(1, std::min(width, frame.cols - left));
        height = std::max(1, std::min(height, frame.rows - top));

        cv::Rect rect(left, top, width, height);
        string label_text;
        if (!ShouldDrawDetection(det, rect, frame, label_text)) {
            continue;
        }
        cv::rectangle(frame, rect, cv::Scalar(0, 0, 255), 2);

        if (!label_text.empty()) {
            int baseLine = 0;
            cv::Size textSize = cv::getTextSize(label_text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);
            cv::rectangle(frame,
                          cv::Point(left, std::max(top - textSize.height - 4, 0)),
                          cv::Point(left + textSize.width, std::max(top, textSize.height + 4)),
                          cv::Scalar(0, 0, 255), cv::FILLED);
            cv::putText(frame, label_text, cv::Point(left, std::max(top - 4, 0)), cv::FONT_HERSHEY_SIMPLEX,
                        0.5, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
        }
    }
}

// A MediaPipe graph is asynchronous: an output packet is not guaranteed to be
// ready immediately after Process2().  Never pass a null packet to LibMP's
// WriteOutputImage(), because that dereferences the packet internally.
static bool CopyMediaPipeOutput(const std::shared_ptr<mediapipe::LibMP>& libmp,
                                const char* stream_name, cv::Mat& destination) {
    if (!libmp || destination.empty() || libmp->GetOutputQueueSize(stream_name) <= 0) {
        return false;
    }

    const void* packet = libmp->GetOutputPacket(stream_name);
    if (packet == nullptr) {
        return false;
    }
    if (mediapipe::LibMP::PacketIsEmpty(packet)) {
        mediapipe::LibMP::DeletePacket(packet);
        return false;
    }
    return mediapipe::LibMP::WriteOutputImage(destination.data, packet);
}

static void EnsureOutputBufferMatches(const cv::Mat& source, cv::Mat& destination) {
    if (destination.rows != source.rows || destination.cols != source.cols || destination.type() != source.type()) {
        destination.create(source.size(), source.type());
    }
}

// for Yolo11n-pose
#include <opencv2/dnn.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "SocketBufferParser.hpp" //DataFrame is defined in this hpp file
#include "ThreadSafeQueue.hpp"

void ThreadProcessImage::SetSettingFile(const QString &filePath) {
    LoadJSONFile(msetting, filePath.toStdString());
}

ThreadProcessImage::ThreadProcessImage() {
    // Initialize the EmotiEffLib
    string backend = "onnx";
    string modelName = EmotiEffLib::getSupportedModels(backend)[0];
    string ext = ".onnx";
    string Homepath(getenv("HOME"));
    string emotiEffLibRootDir = Homepath + "/RobotNurseHelper_build/EmotiEffLib";
    string modelPath = emotiEffLibRootDir + "/models/affectnet_emotions/onnx/" + modelName + ext;
    fer = EmotiEffLib::EmotiEffLibRecognizer::createInstance(backend, modelPath);

    deserialize("dlib_face_recognition_resnet_model_v1.dat") >> net;

    string filepath = std::filesystem::current_path() / "mediapipe_addition/graph_strings/face_cpu.txt";
    string graph_string = LoadFileToString(filepath);
    libmp_face.reset(mediapipe::LibMP::Create(graph_string.c_str(), "input_video"));
    libmp_face->AddOutputStream("multi_face_landmarks");
    libmp_face->AddOutputStream("output_video");
    libmp_face->Start();

    filepath = std::filesystem::current_path() / "mediapipe_addition/graph_strings/hand_cpu.txt";
    graph_string = LoadFileToString(filepath);
    libmp_hand.reset(mediapipe::LibMP::Create(graph_string.c_str(), "input_video"));
    libmp_hand->AddOutputStream("landmarks");
    libmp_hand->Start();

    string poseGraphFileName = "pose_cpu.txt";
    filepath = std::filesystem::current_path() / ("mediapipe_addition/graph_strings/" + poseGraphFileName);
    graph_string = LoadFileToString(filepath);
    libmp_pose.reset(mediapipe::LibMP::Create(graph_string.c_str(), "input_video"));
    libmp_pose->AddOutputStream("pose_landmarks");
    libmp_pose->AddOutputStream("output_video");
    libmp_pose->Start();

    {
        std::vector<std::string> obj_graph_candidates = {
            Homepath + "/mediapipe/mediapipe/graphs/object_detection/object_detection_desktop_live.pbtxt",
            Homepath + "/mediapipe/mediapipe/graphs/object_detection/object_detection_mobile_cpu.pbtxt",
            Homepath + "/mediapipe/mediapipe/graphs/object_detection/object_detection_mobile_gpu.pbtxt"
        };
        string object_graph_path;
        for (const auto& candidate : obj_graph_candidates) {
            if (std::filesystem::exists(candidate)) {
                object_graph_path = candidate;
                break;
            }
        }
        if (!object_graph_path.empty()) {
            string object_graph_string = LoadFileToString(object_graph_path);

            // MediaPipe graph pbtxt often references the labelmap via a relative resource path
            // like: mediapipe/models/ssdlite_object_detection_labelmap.txt
            // At runtime it may not be resolvable from the current working directory.
            // Rewrite it to the absolute path inside this repo.
            string labelmap_src = "mediapipe/models/ssdlite_object_detection_labelmap.txt";
            string labelmap_abs = Homepath + "/RobotNurseHelper/Server/mediapipe/models/ssdlite_object_detection_labelmap.txt";
            string model_src = "mediapipe/models/ssdlite_object_detection.tflite";
            string model_abs = Homepath + "/RobotNurseHelper/Server/mediapipe/models/ssdlite_object_detection.tflite";

            if (object_graph_string.find(labelmap_src) != std::string::npos) {
                // Do a simple in-place replace to avoid dependency on any external helper.
                size_t pos = 0;
                while ((pos = object_graph_string.find(labelmap_src, pos)) != std::string::npos) {
                    object_graph_string.replace(pos, labelmap_src.size(), labelmap_abs);
                    pos += labelmap_abs.size();
                }
            } else {
                std::cerr << "[MediaPipe] labelmap reference not found in object graph: " << object_graph_path
                          << std::endl;
            }

            if (object_graph_string.find(model_src) != std::string::npos) {
                size_t pos = 0;
                while ((pos = object_graph_string.find(model_src, pos)) != std::string::npos) {
                    object_graph_string.replace(pos, model_src.size(), model_abs);
                    pos += model_abs.size();
                }
            }

            // The stock graph is tuned for a phone demo and limits output to
            // three objects at a relatively high confidence.  A care room can
            // contain several people and objects, so retain more detections.
            const std::string old_max_detections = "max_num_detections: 3";
            const std::string new_max_detections = "max_num_detections: 20";
            size_t max_pos = object_graph_string.find(old_max_detections);
            if (max_pos != std::string::npos) {
                object_graph_string.replace(max_pos, old_max_detections.size(), new_max_detections);
            }
            // Let small objects like phones survive the graph-level filter.
            // Class-specific filtering in DrawDetections() keeps noisy labels
            // such as indoor bicycle guesses strict.
            const std::string old_score_threshold = "min_score_thresh: 0.6";
            const std::string new_score_threshold = "min_score_thresh: 0.20";
            size_t score_pos = object_graph_string.find(old_score_threshold);
            if (score_pos != std::string::npos) {
                object_graph_string.replace(score_pos, old_score_threshold.size(), new_score_threshold);
            }

            if (!std::filesystem::exists(model_abs)) {
                std::cerr << "Object detection model is missing: " << model_abs << std::endl;
            } else {
                libmp_object_detector.reset(mediapipe::LibMP::Create(object_graph_string.c_str(), "input_video"));
                if (libmp_object_detector) {
                    libmp_object_detector->AddOutputStream("output_detections");
                    libmp_object_detector->Start();
                    std::cout << "Object detection graph loaded from: " << object_graph_path << std::endl;
                } else {
                    std::cerr << "Failed to create object detection graph." << std::endl;
                }
            }
        } else {
            std::cerr << "Object detection graph not found in local MediaPipe repo." << std::endl;
        }
    }

    string InspireFaceModelPath = Homepath + "/RobotNurseHelper_build/InspireFace/test_res/pack/Pikachu";
    const char *InspireFaceModelPath_cstr = InspireFaceModelPath.c_str();
    if (HFLaunchInspireFace(InspireFaceModelPath_cstr) != HSUCCEED) {
        std::cerr << "InspireFace Initializatino failure!" << std::endl;
    }

    HFSessionCustomParameter customParameter = {0}; // Initial as 0

    customParameter.enable_recognition = 1;
    customParameter.enable_face_attribute = 1;

    // The HF_DETECT_MODE_ALWAYS_DETECT option won't generate TrackID, which is
    // always -1
    HFDetectMode detectMode = HF_DETECT_MODE_TRACK_BY_DETECTION; // HF_DETECT_MODE_ALWAYS_DETECT;
                                                                 // //HF_DETECT_MODE_LIGHT_TRACK;
                                                                 // //HF_DETECT_MODE_TRACK_BY_DETECTION;
                                                                 // //
                                                                 // Use detection-driven tracking mode, suitable for
                                                                 // high-resolution video streams

    HInt32 maxDetectFaceNum = 5;

    HInt32 detectPixelLevel = 320; // default -1 means 320, usually 160、320、640

    HInt32 trackByDetectModeFPS = -1; // if MODE_TRACK_BY_DETECTION, default value -1 means 30fps
    HResult sessionRet = HFCreateInspireFaceSession(customParameter, detectMode, maxDetectFaceNum, detectPixelLevel,
                                                    trackByDetectModeFPS, &session);
    if (sessionRet == HSUCCEED) {
        std::cout << "Session Success" << std::endl;
    } else {
        std::cout << "Session Failure. error code: " << sessionRet << std::endl;
    }

    m_LastPersonDetectedTime = std::chrono::system_clock::now();
    m_LastCompassCheckTime = std::chrono::system_clock::now();
    m_bTurningToZero = false;
    m_bBodyAtZero = false;
}

ThreadProcessImage::~ThreadProcessImage() {
    HFReleaseInspireFaceSession(session);
}

void ThreadProcessImage::run() {
    auto previous_time = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<std::array<float, 3>>> last_landmarks;
    bool bDrawImageByOurOwn = true;
    int iFrameCount = 0;
    int iNoPersonFrameCount = 0; // If cannot find a person for 30 frames, move
                                 // the head to up right frontal
    // try CPU first
    std::vector<std::vector<std::array<float, 3>>> NL_pose; // normalized_landmarks;
    Mat inputImage;                                         // BGR (Blue, Green, Red), this is OpenCV's default color
                                                            // channel order. I will convert it to RGB order before
                                                            // sending it to MediaPipe because MediaPipe uses RGB order.
    chrono::time_point<chrono::system_clock> previous_image_save_time;
    bool frameHasOutput = false;
    while (b_WhileLoop) {
        if (DataFrames_queue.size() > 0) {
            auto start = std::chrono::high_resolution_clock::now();
            DataFrame dataframe;
            // It takes 68 to 138 ms to process a frame, so I need to clear the queue
            // to reduce latency. Maybe it takes less time if I use a faster PC.
            while (DataFrames_queue.size() > 0) {
                DataFrames_queue.pop(dataframe);
            }
            char *data_ = dataframe.data.get();
            size_t data_length = dataframe.length;

            bool bCorrectlyDecoded = false;

            RobotCommandProtobuf::RobotToServerMessage RTSmessage;
            // 2025/12/27 Debug, to parse socket buffer data, I should use
            // ParseFromArray instead of ParseFromString.
            bool parseSuccess = RTSmessage.ParseFromArray(data_, static_cast<int>(data_length));
            if (!parseSuccess) {
                cout << "Failed to parse protobuf message" << endl;
                continue;
            }
            std::vector<uchar> JPEG_Data;
            if (RTSmessage.has_jpegdata() && RTSmessage.has_jpegdatalength()) {
                string strJPEG_Data = RTSmessage.jpegdata();
                if (strJPEG_Data.length() == 0) {
                    cout << "Warning: jpegdata is empty" << endl;
                }
                JPEG_Data.assign(strJPEG_Data.begin(), strJPEG_Data.end());
            } else {
                cout << "No jpegdata or jpegdatalength in the protobuf message" << endl;
                continue;
            }

            try {
                if (JPEG_Data.empty()) {
                    cout << "JPEG_Data is empty, skipping imdecode." << endl;
                    continue;
                }
                inputImage = imdecode(JPEG_Data, IMREAD_COLOR);
                if (inputImage.data) {
                    bCorrectlyDecoded = true;
                } else {
                    cout << "imdecode fails." << endl;
                    continue;
                }
            } catch (exception &e) {
                cout << "imdecode try catch exception: " << e.what() << endl;
            }

            if (bCorrectlyDecoded && !inputImage.empty() && inputImage.cols > 0 && inputImage.rows > 0 && inputImage.data) {
                    // Guard against null/empty frames when stream drops (prevents hard crash)
                    // If the stream disconnects, JPEG decode might succeed intermittently but produce an empty/invalid frame.

                if (iFrameCount == 0) {
                    inputImage.copyTo(outFrame); // To let outFrame has buffer
                    // I draw the outFrame by myself, so I don't need to use the
                    // output_video of MediaPipe. But MediaPipe requires an output buffer
                    // to write the output video. So I create outFrame for this purpose. I
                    // will draw the output video by myself, and I will not use the
                    // content of outFrame outside this function.

                    inputImage.copyTo(tempFrame); // To let tempFrame has buffer
                                                  // tempFrame is used to receive the output video from MediaPipe. I
                                                  // don't need it, but MediaPipe requires an output buffer to write the
                                                  // output video. So I create tempFrame for this purpose. I will not
                                                  // use the content of tempFrame outside this function.
                }

                bool bSaveProcessResult = false; // default false, this variable is
                                                 // controlled by the iFrameCount
                if (bSaveTransmittedImage) {
                    chrono::time_point<chrono::system_clock> protobufTimestamp =
                        protobufTimestampToTimePoint(RTSmessage.event_time());
                    if (iFrameCount == 0 || protobufTimestamp - previous_image_save_time >
                                                chrono::milliseconds(msetting.iImageSaveIntervalMillisecond)) {
                        const bool bMillisecond = true;
                        mstr_captured_timestamp =
                            ConvertProtobufTimestampToString(RTSmessage.event_time(),
                                                             bMillisecond); // use event_time as the timestamp for
                                                                            // saving images. It is more accurate than
                                                                            // using the current time.
                        string filename = ImageSaveDirectory + "/" + mstr_captured_timestamp + ".jpg";
                        previous_image_save_time = protobufTimestamp;
                        if (!m_bDirectoryCreated) {
                            if (!CheckDirectoryExist(ImageSaveDirectory)) {
                                CreateDirectory(ImageSaveDirectory);
                                m_bDirectoryCreated = true;
                            }
                        }
                        save_image_JPEG(JPEG_Data, filename); // wrong color.
                        bSaveProcessResult = true;
                    }
                }

                if (bSaveRequestedPhoto) {
                    bSaveRequestedPhoto = false;
                    const bool bMillisecond = true;
                    mstr_captured_timestamp = ConvertProtobufTimestampToString(RTSmessage.event_time(), bMillisecond);
                    string ImageSaveDirectory_VisualCompass = ImageSaveDirectory + "/VisualCompass/";
                    if (!m_bDirectoryCreated_VisualCompass) {
                        if (!CheckDirectoryExist(ImageSaveDirectory_VisualCompass)) {
                            CreateDirectory(ImageSaveDirectory_VisualCompass);
                        }
                        m_bDirectoryCreated_VisualCompass = CheckDirectoryExist(ImageSaveDirectory_VisualCompass);
                    }
                    string filename = ImageSaveDirectory_VisualCompass + requestedPhotoPrefix + "_" +
                                      mstr_captured_timestamp + ".jpg";
                    save_image_JPEG(JPEG_Data, filename);
                    if (std::filesystem::exists(filename) && std::filesystem::file_size(filename) > 0) {
                        m_bVisualCompassReferencePhotosUnavailable = false;
                        cout << "Manual requested photo saved: " << filename << endl;
                    } else {
                        std::cerr << "Could not save VisualCompass reference photo: " << filename << std::endl;
                    }
                }

                // Draw Pose landmarks
                std::unique_lock<std::mutex> update_frame_lock(mtx_UpdateOutFrame);
                std::unique_lock<std::mutex> task_lock(mtx_Task);
                // ToDo: remove this variable.
                // if( b_HumanPoseEstimation)
                frameHasOutput = false;
                if (msetting.bHumanPoseEstimation) {
                    bool use_Yolo11n_Pose = true;
                    if (msetting.PoseEstimationModel == "Yolo11n_Pose") {
                        use_Yolo11n_Pose = true;
                    } else if (msetting.PoseEstimationModel == "MediaPipe_Pose") {
                        use_Yolo11n_Pose = false;
                    } else {
                        cout << "Unknown PoseEstimationModel: " << msetting.PoseEstimationModel
                             << ". Use Yolo11n_Pose by default." << endl;
                        use_Yolo11n_Pose = true;
                    }

                    if (use_Yolo11n_Pose) {
                        NL_pose = yolo11pose.Process(inputImage); // process the inputImage and
                                                                  // draw the pose on inputImage
                    } else {
                        // The incoming JPEG decodes to a CPU cv::Mat.  Use the CPU
                        // graph and Process2() consistently; feeding an ImageFrame to
                        // a GPU graph can terminate this processing thread.
                        if (!libmp_pose->Process2(inputImage)) {
                            std::cerr << "libmp_pose Process2() failed; dropping this pose result." << std::endl;
                        }

                    }



                    // 2025 Nov 5. Debug: MediaPipe cannot run GPU and CPU at the same
                    // time.
                    if (use_Yolo11n_Pose) {
                        int num_kps = 17; // for pose
                        const float KP_CONF_THRES = 0.4f;
                        inputImage.copyTo(outFrame);
                        size_t num_poses = NL_pose.size();
                        for (int pose_num = 0; pose_num < num_poses; pose_num++) {
                            // draw skeleton
                            for (auto &pr : yolo11pose.skeleton) {
                                int a = pr.first;
                                int b2 = pr.second;
                                float kp_conf_a = NL_pose[pose_num][a][2];
                                float kp_conf_b2 = NL_pose[pose_num][b2][2];
                                if (a < num_kps && b2 < num_kps && kp_conf_a > KP_CONF_THRES &&
                                    kp_conf_b2 > KP_CONF_THRES) {
                                    cv::Scalar col = yolo11pose.pair_color(a, b2);
                                    int a_x = static_cast<int>(NL_pose[pose_num][a][0] * inputImage.cols);
                                    int a_y = static_cast<int>(NL_pose[pose_num][a][1] * inputImage.rows);
                                    int b2_x = static_cast<int>(NL_pose[pose_num][b2][0] * inputImage.cols);
                                    int b2_y = static_cast<int>(NL_pose[pose_num][b2][1] * inputImage.rows);
                                    cv::line(outFrame, cv::Point(a_x, a_y), cv::Point(b2_x, b2_y), col, 3, cv::LINE_AA);
                                }
                            }
                        }
                        bNewoutFrame = true;
                    } else {
                        EnsureOutputBufferMatches(inputImage, outFrame);
                        if (CopyMediaPipeOutput(libmp_pose, "output_video", outFrame)) {
                            frameHasOutput = true;
                            bNewoutFrame = true;
                        }

                        NL_pose = get_landmarks_pose(libmp_pose); // I use this to guide robot's movement
                        //260702 How to get the confidence of each landmark?
                        //No, the 3rd column is not the confidence. It is the z coordinate.
                        //for (int i = 0; i < 3; i++) {
                        //    cout << "Landmark " << i << ": " << NL_pose[0][i][0] << ", " << NL_pose[0][i][1] << ", " << NL_pose[0][i][2] << endl;
                        //}
                    }
                }

                // Draw hand landmarks
                if (msetting.bHandLandmarkDetection) {
                    if (!libmp_hand->Process2(inputImage)) {
                        std::cerr << "libmp_hand Process2() failed; skipping hand landmarks for this frame." << std::endl;
                    }

                    std::vector<std::vector<std::array<float, 3>>> NL_hands; // normalized_landmarks;
                    NL_hands = get_landmarks_hand(libmp_hand);
                    if (bDrawImageByOurOwn) {
                        if (!frameHasOutput) {
                            inputImage.copyTo(outFrame);
                            frameHasOutput = true;
                        }
                        static const std::vector<std::pair<int, int>> hand_connections = {
                            {0, 1}, {1, 2}, {2, 3}, {3, 4},     // Thumb
                            {0, 5}, {5, 6}, {6, 7}, {7, 8},     // Index
                            {0, 9}, {9, 10}, {10, 11}, {11, 12}, // Middle
                            {0, 13}, {13, 14}, {14, 15}, {15, 16}, // Ring
                            {0, 17}, {17, 18}, {18, 19}, {19, 20}  // Pinky
                        };

                        size_t num_hands = NL_hands.size();
                        for (int hand_num = 0; hand_num < num_hands; hand_num++) {
                            const auto &hand_landmarks = NL_hands[hand_num];
                            std::vector<cv::Point> points;
                            points.reserve(hand_landmarks.size());
                            for (const std::array<float, 3> &norm_xyz : hand_landmarks) {
                                int x = static_cast<int>(norm_xyz[0] * inputImage.cols);
                                int y = static_cast<int>(norm_xyz[1] * inputImage.rows);
                                points.emplace_back(x, y);
                            }

                            // Draw skeleton lines for the hand
                            for (const auto &connection : hand_connections) {
                                int a = connection.first;
                                int b = connection.second;
                                if (a < static_cast<int>(points.size()) && b < static_cast<int>(points.size())) {
                                    cv::line(outFrame, points[a], points[b], cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
                                }
                            }

                            // Draw each landmark as a dot
                            for (const cv::Point &pt : points) {
                                cv::circle(outFrame, pt, 4, cv::Scalar(0, 0, 255), -1);
                            }
                        }
                    }
                }

                std::vector<std::vector<std::array<float, 3>>> NL_faces; // normalized_landmarks;
                if (msetting.bFaceDetection) {
                    if (msetting.FaceDetectionModel == "InspireFace") {
                        // Use InspireFace to detect faces.
                        HFImageBitmapData imageBitmapData;
                        imageBitmapData.data = inputImage.data;
                        imageBitmapData.width = inputImage.cols;
                        imageBitmapData.height = inputImage.rows;
                        imageBitmapData.channels = inputImage.channels();

                        HResult result;
                        HFImageBitmap image;
                        result = HFCreateImageBitmap(&imageBitmapData, &image);
                        if (result != HSUCCEED) {
                            std::cerr << "Failed to create image bitmap." << std::endl;
                            break;
                        }

                        HFImageStream imageHandle = {0};
                        result = HFCreateImageStreamFromImageBitmap(image, HF_CAMERA_ROTATION_0, &imageHandle);
                        if (result != HSUCCEED) {
                            std::cerr << "Failed to set image format." << std::endl;
                            break;
                        }

                        result = HFSessionSetFaceDetectThreshold(
                            session,
                            0.5); // Optional: Set confidence threshold for face detection, range is 0.0 to 1.0
                        if (result != HSUCCEED) {
                            std::cerr << "Failed to set face detection threshold." << std::endl;
                            break;
                        }

                        HFMultipleFaceData MultipleFaceData = {0};
                        result = HFExecuteFaceTrack(session, imageHandle, &MultipleFaceData);
                        if (result != HSUCCEED) {
                            std::cerr << "Failed to execute face tracking." << std::endl;
                            break;
                        }

                        if (MultipleFaceData.detectedNum > 0) {
                            HFSessionCustomParameter customParameter = {0}; // Initialize all to 0
                            // 2. Enable features as needed (1 is enabled, 0 is disabled)
                            customParameter.enable_recognition = 1; // Enable face recognition (feature extraction)
                            customParameter.enable_liveness = 0;    // Disable RGB liveness detection
                            customParameter.enable_face_attribute =
                                1; // Enable face attribute analysis (e.g., gender, age)
                            customParameter.enable_ir_liveness = 0; // Disable IR liveness detection

                            result =
                                HFMultipleFacePipelineProcess(session, imageHandle, &MultipleFaceData, customParameter);
                            if (result != HSUCCEED) {
                                std::cerr << "Failed to process multiple face pipeline." << std::endl;
                                break;
                            }

                            HFFaceAttributeResult faceAttributeResult;
                            HResult result = HFGetFaceAttributeResult(session, &faceAttributeResult);
                            if (result == HSUCCEED) {
                                string FaceGender = "";
                                string FaceAge = "";
                                for (int i = 0; i < faceAttributeResult.num; i++) {
                                    if (faceAttributeResult.gender[i] == 0) {
                                        FaceGender = "F";
                                        msPatientGender = "Female";
                                    } else {
                                        FaceGender = "M";
                                        msPatientGender = "Male";
                                    }
                                    ///< 0: 0-2 years old;
                                    ///< 1: 3-9 years old;
                                    ///< 2: 10-19 years old;
                                    ///< 3: 20-29 years old;
                                    ///< 4: 30-39 years old;
                                    ///< 5: 40-49 years old;
                                    ///< 6: 50-59 years old;
                                    ///< 7: 60-69 years old;
                                    ///< 8: more than 70 years old;
                                    switch (faceAttributeResult.ageBracket[i]) {
                                    case 0:
                                        FaceAge = "0-2";
                                        miPatientAge = 1;
                                        break;
                                    case 1:
                                        FaceAge = "3-9";
                                        miPatientAge = 6;
                                        break;
                                    case 2:
                                        FaceAge = "10-19";
                                        miPatientAge = 15;
                                        break;
                                    case 3:
                                        FaceAge = "20-29";
                                        miPatientAge = 25;
                                        break;
                                    case 4:
                                        FaceAge = "30-39";
                                        miPatientAge = 35;
                                        break;
                                    case 5:
                                        FaceAge = "40-49";
                                        miPatientAge = 45;
                                        break;
                                    case 6:
                                        FaceAge = "50-59";
                                        miPatientAge = 55;
                                        break;
                                    case 7:
                                        FaceAge = "60-69";
                                        miPatientAge = 65;
                                        break;
                                    case 8:
                                        FaceAge = "70+";
                                        miPatientAge = 75;
                                        break;
                                    default:
                                        FaceAge = "Unknown";
                                    }

                                    HFaceRect faceRect = MultipleFaceData.rects[i];
                                    cv::rectangle(
                                        outFrame, cv::Point(faceRect.x, faceRect.y),
                                        cv::Point(faceRect.x + faceRect.width - 1, faceRect.y + faceRect.height - 1),
                                        cv::Scalar(0, 255, 0), 2);
                                    // cout << "MultipleFaceData.trackIds[i]: " <<
                                    // MultipleFaceData.trackIds[i] << endl;
                                    cv::putText(outFrame,
                                                std::to_string(MultipleFaceData.trackIds[i]) + " " + FaceGender + " " +
                                                    FaceAge,
                                                cv::Point(faceRect.x, faceRect.y - 10), cv::FONT_HERSHEY_SIMPLEX, 1,
                                                cv::Scalar(0, 255, 0), 2);
                                }
                            } else {
                                cout << "Failed to get face attribute result, error code: " << result << endl;
                            }
                        }
                        HFReleaseImageStream(imageHandle);
                    } else if (msetting.FaceDetectionModel == "MediaPipe_Face") {
                        // Use MediaPipe to detect faces.
                        if (!libmp_face->Process2(inputImage)) {
                            std::cerr << "libmp_face Process2() failed; skipping face landmarks for this frame." << std::endl;
                        }

                        // Draw face
                        // Do I need the output_video of libmp_face? I only need the
                        // landmarks. I draw the MediaPipe output to tempFrame, which is not
                        // used outside this function.
                        EnsureOutputBufferMatches(inputImage, tempFrame);
                        if (CopyMediaPipeOutput(libmp_face, "output_video", tempFrame)) {
                            bNewoutFrame = true;
                        }
                        NL_faces = get_landmarks_face(libmp_face);
                        if (bDrawImageByOurOwn) {
                            size_t num_faces = NL_faces.size();
                            for (int face_num = 0; face_num < num_faces; face_num++) {
                                const auto &face_landmarks = NL_faces[face_num];
                                for (const std::array<float, 3> &norm_xyz : face_landmarks) {
                                    int x = static_cast<int>(norm_xyz[0] * inputImage.cols);
                                    int y = static_cast<int>(norm_xyz[1] * inputImage.rows);
                                    cv::circle(outFrame, cv::Point(x, y), 1, cv::Scalar(0, 255, 0), -1);
                                }

                                if (!frameHasOutput) {
                            inputImage.copyTo(outFrame);
                            frameHasOutput = true;
                        }
                        static const int left_eye_indices[] = {33, 133, 246, 161, 160, 159, 158, 157, 173};
                                static const int right_eye_indices[] = {362, 263, 390, 389, 388, 387, 386, 385, 384};
                                cv::Point left_eye_center(0, 0);
                                cv::Point right_eye_center(0, 0);
                                int left_count = 0;
                                int right_count = 0;

                                for (int idx : left_eye_indices) {
                                    if (idx < static_cast<int>(face_landmarks.size())) {
                                        left_eye_center.x += static_cast<int>(face_landmarks[idx][0] * inputImage.cols);
                                        left_eye_center.y += static_cast<int>(face_landmarks[idx][1] * inputImage.rows);
                                        left_count++;
                                    }
                                }
                                for (int idx : right_eye_indices) {
                                    if (idx < static_cast<int>(face_landmarks.size())) {
                                        right_eye_center.x += static_cast<int>(face_landmarks[idx][0] * inputImage.cols);
                                        right_eye_center.y += static_cast<int>(face_landmarks[idx][1] * inputImage.rows);
                                        right_count++;
                                    }
                                }

                                if (left_count > 0) {
                                    left_eye_center.x /= left_count;
                                    left_eye_center.y /= left_count;
                                    cv::circle(outFrame, left_eye_center, 4, cv::Scalar(0, 0, 255), -1);
                                }
                                if (right_count > 0) {
                                    right_eye_center.x /= right_count;
                                    right_eye_center.y /= right_count;
                                    cv::circle(outFrame, right_eye_center, 4, cv::Scalar(0, 0, 255), -1);
                                }
                                if (left_count > 0 && right_count > 0) {
                                    cv::line(outFrame, left_eye_center, right_eye_center, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
                                }
                            }
                        }

                        //                    if( m_bRecognizeFacialExpression &&
                        //                    !NL_faces.empty() )
                        if (!NL_faces.empty()) {
                            size_t num_faces = NL_faces.size();
                            for (int face_num = 0; face_num < num_faces; face_num++) {
                                // crop the face region.
                                // Why is the cropped region too small?
                                Mat face = CropRegion(inputImage, NL_faces[face_num]);
                                // Debug 2025 Nov 5: Here is the reason that Hinton frozens.
                                // I sitll don't know why. But if I disable this imshow(),
                                // Hinton works fine.
                                //                            cv::imshow("Cropped face", face);
                                if (msetting.bFacialExpressionRecognition) {
                                    auto res =
                                        fer->predictEmotions(face, false); // false will return the softmax scores
                                    Rect roi = GetBoundingBoxFromLandmarks(NL_faces[face_num], inputImage.cols,
                                                                           inputImage.rows);
                                    // not very accurate, the cropped face is too small, around
                                    // 135x156.
                                    //                            cout << "Cropped size: " <<
                                    //                            face.cols << " " << face.rows <<
                                    //                            endl;
                                    // std::format requires C++20, but my project uses C++17. So I
                                    // use sprintf instead.
                                    //                            cv::putText(outFrame,
                                    //                            res.labels[0] +
                                    //                            std::format("{:.3f}",
                                    //                            res.scores[0]) , Point(roi.x,
                                    //                            roi.y) ,
                                    //                            cv::FONT_HERSHEY_SIMPLEX, 1. ,
                                    //                            cv::Scalar(0,255,0), 2);
                                    char text[256];
                                    sprintf(text, "%s%.3f", res.labels[0].c_str(), res.scores[0]);
                                    cv::putText(outFrame, text, Point(roi.x, roi.y), cv::FONT_HERSHEY_SIMPLEX, 1.,
                                                cv::Scalar(0, 255, 0), 2);
                                }

                                // Get face recognition features
                                // Although there is only one face, the dlib face recognition
                                // model needs a vector of faces as input.
                                if (msetting.bUseDlibForFaceRecognition) {
                                    std::vector<dlib::matrix<dlib::rgb_pixel>> faces;
                                    dlib::matrix<dlib::rgb_pixel> dlib_face;
                                    // The face size has to be 150x150, which is the input size of
                                    // the dlib face recognition model. So I need to resize the
                                    // cropped face to 150x150 before sending it to the dlib
                                    // model. But resizing may cause distortion, so I will skip
                                    // this step if the face size is not correct.
                                    if (face.cols != 150 || face.rows != 150) {
                                        // Use OpenCV to resize the face to 150x150. But it may
                                        // cause distortion. I will skip this step if the face size
                                        // is not correct.
                                        Mat resized_face;
                                        cv::resize(face, resized_face, Size(150, 150));
                                        face = resized_face;
                                    }
                                    dlib::assign_image(dlib_face, dlib::cv_image<dlib::bgr_pixel>(face));
                                    faces.push_back(dlib_face);
                                    // dlib use GPU already, but it is still slow.
                                    // std::cout << "CUDA Device Count: " <<
                                    // dlib::cuda::get_num_devices() << std::endl;
                                    std::vector<matrix<float, 0, 1>> face_descriptors = net(faces);
                                    // It is a vector of 128D
                                    // print it out
                                    cout << "face descriptor for one face: " << dlib::trans(face_descriptors[0])
                                         << endl;
                                }
                                // how to create a cluster of the face descriptors for face
                                // recognition? no idea now.
                            }
                        }

                    } else {
                        cout << "Unknown FaceDetectionModel: " << msetting.FaceDetectionModel
                             << ". Use InspireFace by default." << endl;
                    }
                }

                if (msetting.bObjectDetection && libmp_object_detector) {
                    if (!libmp_object_detector->Process2(inputImage)) {
                        std::cerr << "libmp_object_detector Process() failed!" << std::endl;
                    } else {
                        std::vector<mediapipe::Detection> detections = GetDetectionsFromPacket(libmp_object_detector, "output_detections");
                        if (!detections.empty()) {
                            if (!frameHasOutput) {
                                inputImage.copyTo(outFrame);
                                frameHasOutput = true;
                            }
                            DrawDetections(outFrame, detections);
                            bNewoutFrame = true;
                        }
                    }
                }

                // Dump outFrame for debugging
                if (bSaveProcessResult) {
                    string filename = ImageSaveDirectory + "/" + mstr_captured_timestamp + ".outFrame.jpg";
                    cv::imwrite(filename, outFrame);

                    size_t num_faces = NL_faces.size();
                    for (int face_num = 0; face_num < num_faces; face_num++) {
                        Mat face = CropRegion(inputImage, NL_faces[face_num]);
                        filename =
                            ImageSaveDirectory + "/" + mstr_captured_timestamp + ".face" + to_string(face_num) + ".jpg";
                        cv::imwrite(filename, face);
                    }
                }

                update_frame_lock.unlock();

                // This variable is used to prevent the robot from sending new commands
                // while the previous command is being executed.
                if (mbWatchPatient) {
                    if (!NL_pose.empty()) // If there is no person detected, the following
                                          // code will not be executed.
                    {
                        iNoPersonFrameCount = 0;
                        m_LastPersonDetectedTime = std::chrono::system_clock::now();
                        m_bTurningToZero = false;
                        m_bBodyAtZero = false;

                        // use time control
                        auto current_time = chrono::high_resolution_clock::now();
                        auto duration = chrono::duration_cast<chrono::seconds>(current_time - previous_time);
                        // to prevent too many messages being sent to the robot, I set a
                        // time interval between two messages.
                        if (duration.count() >= 1) {
                            if (action_option.move_mode != action_option.MOVE_MANUAL) {
                                RobotCommandProtobuf::RobotCommand command;
                                if (msetting.PoseEstimationModel == "Yolo11n_Pose") {
                                    PoseLandmarks_to_RobotAction_yolo(NL_pose, robot_status, action_option, command);
                                } else {
                                    PoseLandmarks_to_RobotAction(NL_pose, robot_status, action_option, command);
                                }
                                previous_time = current_time;
                                pSendMessageManager->AddMessage(command); // The command is filled in the
                                                                          // PoseLandmarks_to_RobotAction function
                            }
                        }
                    } else {
                        iNoPersonFrameCount++;
                        if (iNoPersonFrameCount > 30) {
                            RobotCommandProtobuf::RobotCommand command;
                            command.set_yaw(0);
                            command.set_pitch(0);
                            pSendMessageManager->AddMessage(command);
                            // debug
                            iNoPersonFrameCount = 0;
                        }

                        // Return to zero orientation using Visual Compass if no human detected for 1 second
                        auto now = std::chrono::system_clock::now();
                        auto elapsed_no_person =
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastPersonDetectedTime)
                                .count();
                        if (action_option.bUseVisualCompass && !m_bVisualCompassReferencePhotosUnavailable &&
                            elapsed_no_person >= 1000 && !m_bBodyAtZero) {
                            m_bTurningToZero = true;
                            auto elapsed_compass =
                                std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastCompassCheckTime)
                                    .count();
                            if (elapsed_compass >= 500) {
                                m_LastCompassCheckTime = now;       //Check VisualCompass every 500ms. The interval may be too long. There is a delay of several hundred ms for the robot to turn.
                                std::string errorDetails;
                                double estimatedAngle =
                                    ::ComputeVisualCompassTheta(inputImage, ImageSaveDirectory, errorDetails);
                                //print the estimated theta out
                                std::cout << "Estimated theta: " << estimatedAngle << std::endl;
                                if (estimatedAngle >= 0.0) {
                                    float turnSpeed = 0.0f;
                                    if (estimatedAngle > 5.0 && estimatedAngle <= 180.0) {
                                        turnSpeed = -30.0f; // Turn right (clockwise)
                                    } else if (estimatedAngle > 180.0 && estimatedAngle < 355.0) {
                                        turnSpeed = 30.0f; // Turn left (counter-clockwise)
                                    } else {
                                        turnSpeed = 0.0f; // Already at 0 degree (within 5 degree tolerance)
                                        m_bBodyAtZero = true;
                                        m_bTurningToZero = false;
                                    }
                                    RobotCommandProtobuf::RobotCommand command;
                                    command.set_turnspeed(turnSpeed);           //This is Kebbi's limitation, which can only control the robot to turn at a constant speed, not the angle to turn.
                                    pSendMessageManager->AddMessage(command);
                                } else {
                                    // Missing references cannot succeed until a user captures them.
                                    // Suppress the repeated error and the repeated -1 estimates instead
                                    // of attempting the same lookup twice per second.
                                    if (errorDetails.find("No saved photos found") != std::string::npos ||
                                        errorDetails.find("directory does not exist") != std::string::npos) {
                                        m_bVisualCompassReferencePhotosUnavailable = true;
                                        std::cerr << "VisualCompass disabled until reference photos are captured: "
                                                  << errorDetails << std::endl;
                                    } else {
                                        std::cerr << "VisualCompass failed to determine orientation: " << errorDetails
                                                  << std::endl;
                                    }
                                }
                            }
                        }
                    }
                } // if( mbWatchPatient )
                task_lock.unlock();
            } // if( bCorrectlyDecoded )

            // debug code, to messure the processing time
            bool bShowTransmittedImage = false;
            if (bShowTransmittedImage) {
                auto stop = std::chrono::high_resolution_clock::now();
                auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
                std::cout << "Elapsed time: " << duration_ms.count() << " milliseconds" << std::endl;
            }

            iFrameCount++;
        } // if( pSocketBufferParser->get_queue_length() > 0 )
        msleep(1); // to prevent CPU usage too high
    } // while(b_WhileLoop)
    cout << "Exit ThreadProcessImage loop." << endl;
}

void ThreadProcessImage::NotifyEvent(string description, chrono::time_point<chrono::system_clock> timestamp, float yaw,
                                     float pitch) {
    if (description == "onCompleteOfMotionPlay") {
        //        cout << "(A) onCompleteOfMotionPlay yaw " << yaw << " pitch " <<
        //        pitch << endl;
        robot_status.yaw_degree = (int)yaw;
        robot_status.pitch_degree = (int)pitch;
        mbWatchPatient = true;
    } else if (description == "KebbiMoveHeadDuringMotion") {
        //        cout << "(B) KebbiMoveHeadDuringMotion " << endl;
        mbWatchPatient = false;
    }
}

Mat ThreadProcessImage::getOutFrame() {
    Mat frame;
    mtx_UpdateOutFrame.lock();
    if (bNewoutFrame) {
        outFrame.copyTo(frame);
    }
    mtx_UpdateOutFrame.unlock();
    return frame;
}

Mat ThreadProcessImage::CropRegion(Mat inputImage, std::vector<std::array<float, 3>> normalized_landmarks) {
    Rect roi = GetBoundingBoxFromLandmarks(normalized_landmarks, inputImage.cols, inputImage.rows);
    Mat cropped_face = inputImage(roi).clone(); // clone to ensure a deep copy
    return cropped_face;
}

cv::Rect ThreadProcessImage::GetBoundingBoxFromLandmarks(const std::vector<std::array<float, 3>> &normalized_landmarks,
                                                         int img_width, int img_height) {
    // Find the bounding box of the landmarks
    float x_min = 1.0, x_max = 0.0, y_min = 1.0, y_max = 0.0;
    for (const auto &norm_xyz : normalized_landmarks) {
        if (norm_xyz[0] < x_min)
            x_min = norm_xyz[0];
        if (norm_xyz[0] > x_max)
            x_max = norm_xyz[0];
        if (norm_xyz[1] < y_min)
            y_min = norm_xyz[1];
        if (norm_xyz[1] > y_max)
            y_max = norm_xyz[1];
    }

    // Convert to pixel coordinates
    int x1 = static_cast<int>(x_min * img_width);
    int y1 = static_cast<int>(y_min * img_height);
    int x2 = static_cast<int>(x_max * img_width);
    int y2 = static_cast<int>(y_max * img_height);

    // Ensure the bounding box is within image boundaries
    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 > img_width)
        x2 = img_width;
    if (y2 > img_height)
        y2 = img_height;

    // Return the bounding box
    return Rect(x1, y1, x2 - x1, y2 - y1);
}

string ThreadProcessImage::GetPatientGender() {
    return msPatientGender;
}

int ThreadProcessImage::GetPatientAge() {
    return miPatientAge;
}

Mat ThreadProcessImage::getLatestFrame() {
    Mat frame;
    mtx_UpdateOutFrame.lock();
    outFrame.copyTo(frame);
    mtx_UpdateOutFrame.unlock();
    return frame;
}
