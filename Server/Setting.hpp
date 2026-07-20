#ifndef SETTING_HPP
#define SETTING_HPP

#include <string>
#include <nlohmann/json.hpp>
#include "utility_json.hpp"

using namespace std;

struct Setting
{
    string StateControlFile;
    bool bServerPlaysRobotReceivedAudio = false;
    bool bVideoWindowFullScreen = true;
    bool bShowPreviewWindow = false;
    bool bSaveImages = false;
    int iImageSaveIntervalMillisecond = 1000;   //default value is 1000, which means the interval between two save images is 1000 milliseconds. This is used to control how many images will be saved when bSaveTransmittedImage is true. The smaller this value, the more images will be saved.
    bool bFacialExpressionRecognition = false;
    bool bHumanPoseEstimation = false;
    string PoseEstimationModel = "Yolo11n_Pose";  //Yolo11n_Pose, MediaPipe_Pose
    bool bHandLandmarkDetection = false;
    bool bUseDlibForFaceRecognition = false;
    bool bFaceDetection = false;
    string FaceDetectionModel = "InspireFace";    //MediaPipe_Face, InspireFace
    // Object detection is optional. Keep it off unless a supported detector is
    // explicitly configured, so a missing/incompatible graph cannot stop video.
    bool bObjectDetection = false;
    string ObjectDetectionModel = "MediaPipe_Object"; // MediaPipe_Object, MediaPipe_ObjectDetectorTask
    string ObjectTaskPath;
    bool bUseVisualCompass = false;
    string WhisperModel = "$HOME/RobotNurseHelper_build/whisper.cpp/models/ggml-large-v3-turbo.bin";
    string ImageSaveDirectory = "$HOME/Downloads/raw_images";
    string LanguageModel = "gemma3:1b";
    string Language = "Chinese";
    string AnythingLLM_API_key;
    string AnythingLLM_workspace_slug;
    // Path to the YOLO ONNX model used for object detection
    string YoloModelPath = "";
    bool bHideCursor = true;
    string Machine = "PC";                      //AGXOrin, PC
};

inline void from_json(const nlohmann::json& j, Setting& s) {
    s.StateControlFile = j.value("StateControlFile", s.StateControlFile);
    s.bServerPlaysRobotReceivedAudio = j.value("bServerPlaysRobotReceivedAudio", s.bServerPlaysRobotReceivedAudio);
    s.bVideoWindowFullScreen = j.value("bVideoWindowFullScreen", s.bVideoWindowFullScreen);
    s.bShowPreviewWindow = j.value("bShowPreviewWindow", s.bShowPreviewWindow);
    s.bSaveImages = j.value("bSaveImages", s.bSaveImages);
    s.iImageSaveIntervalMillisecond = j.value("iImageSaveIntervalMillisecond", s.iImageSaveIntervalMillisecond);
    s.bFacialExpressionRecognition = j.value("bFacialExpressionRecognition", s.bFacialExpressionRecognition);
    s.bHumanPoseEstimation = j.value("bHumanPoseEstimation", s.bHumanPoseEstimation);
    s.PoseEstimationModel = j.value("PoseEstimationModel", s.PoseEstimationModel);
    s.bHandLandmarkDetection = j.value("bHandLandmarkDetection", s.bHandLandmarkDetection);
    s.bUseDlibForFaceRecognition = j.value("bUseDlibForFaceRecognition", s.bUseDlibForFaceRecognition);
    s.bFaceDetection = j.value("bFaceDetection", s.bFaceDetection);
    s.FaceDetectionModel = j.value("FaceDetectionModel", s.FaceDetectionModel);
    s.bObjectDetection = j.value("bObjectDetection", s.bObjectDetection);
    s.ObjectDetectionModel = j.value("ObjectDetectionModel", s.ObjectDetectionModel);
    s.ObjectTaskPath = j.value("ObjectTaskPath", s.ObjectTaskPath);
    s.bUseVisualCompass = j.value("bUseVisualCompass", s.bUseVisualCompass);
    s.WhisperModel = j.value("WhisperModel", s.WhisperModel);
    s.ImageSaveDirectory = j.value("ImageSaveDirectory", s.ImageSaveDirectory);
    s.LanguageModel = j.value("LanguageModel", s.LanguageModel);
    s.Language = j.value("Language", s.Language);
    s.AnythingLLM_API_key = j.value("AnythingLLM_API_key", s.AnythingLLM_API_key);
    s.AnythingLLM_workspace_slug = j.value("AnythingLLM_workspace_slug", s.AnythingLLM_workspace_slug);
    s.YoloModelPath = j.value("YoloModelPath", s.YoloModelPath);
    s.bHideCursor = j.value("bHideCursor", s.bHideCursor);
    s.Machine = j.value("Machine", s.Machine);
}

#endif // SETTING_HPP
