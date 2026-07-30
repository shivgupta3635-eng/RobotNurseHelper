#include "ThreadStateControl.hpp"
#include <fstream>
#include "utility_time.hpp"

#include "BargeInControl.hpp"


#include <cstdlib> // For rand() and srand()
#include <ctime>   // For time()
#include <sstream>
#include <algorithm>
#include "utility_json.hpp"
#include "ollama.hpp"
#include <filesystem>

ThreadStateControl::ThreadStateControl()
{

}

ThreadStateControl::~ThreadStateControl()
{
    
}

void ThreadStateControl::SetSettingFile(const QString &filePath)
{
    LoadJSONFile(msetting, filePath.toStdString());
    if (msetting.StateControlFile.find("_English.json") != std::string::npos ||
        msetting.StateControlFile.find("eng_") != std::string::npos) {
        msetting.Language = "English";
    } else if (msetting.StateControlFile.find("Chinese") != std::string::npos ||
               msetting.StateControlFile == "json/Cataractproject.json" ||
               msetting.StateControlFile == "json/Cataract.json") {
        msetting.Language = "Chinese";
    }
    //After loading the setting, I can set the initial state of the state control thread.
    InitializeStates();
}

void ThreadStateControl::InitializeStates()
{
    LoadJSONFile(mStates, msetting.StateControlFile);
    for( size_t i = 0; i < mStates.size(); i++)
    {
        mStates[i].m_secDurationLimit = chrono::seconds(mStates[i].iDurationLimitSeconds);
    }
    LoadPatientTitles();

    // If language is English, optionally create an eng_ copy of the state control
    // JSON where image paths are replaced with eng_images/ to help maintain a
    // separate file as the user requested.
    try {
        if (msetting.Language == "English") {
            namespace fs = std::filesystem;
            std::string orig = msetting.StateControlFile;
            fs::path p(orig);
            if (fs::exists(p)) {
                std::ifstream inFile(orig);
                if (inFile) {
                    std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
                    inFile.close();
                    // Replace Images/ with eng_images/
                    bool replaced = false;
                    std::string from = "Images/";
                    std::string to = "eng_images/";
                    size_t pos = 0;
                    while ((pos = content.find(from, pos)) != std::string::npos) {
                        content.replace(pos, from.length(), to);
                        pos += to.length();
                        replaced = true;
                    }
                    if (replaced) {
                        fs::path newPath = p.parent_path() / (std::string("eng_") + p.filename().string());
                        std::ofstream outFile(newPath.string());
                        if (outFile) {
                            outFile << content;
                            outFile.close();
                            cout << "Created English state control copy: " << newPath.string() << endl;
                        } else {
                            cout << "Warning: failed to write eng copy: " << newPath.string() << endl;
                        }
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        cout << "Warning: exception when creating eng_ JSON copy: " << e.what() << endl;
    }
}

std::string ThreadStateControl::GetLocalizedImagePath(const std::string &originalPath)
{
    // If language is English and image paths reference the default Images/ folder,
    // switch to eng_images/ so English images are used when available.
    if (msetting.Language == "English") {
        namespace fs = std::filesystem;
        const std::string prefix1 = "Images/";
        const std::string prefix2 = "images/";
        std::string name;
        if (originalPath.rfind(prefix1, 0) == 0) {
            name = originalPath.substr(prefix1.size());
        } else if (originalPath.rfind(prefix2, 0) == 0) {
            name = originalPath.substr(prefix2.size());
        } else {
            // If originalPath doesn't start with Images/, just use the basename
            fs::path p(originalPath);
            name = p.filename().string();
        }

        // Candidate 1: eng_images/<name>
        fs::path cand1 = fs::path("eng_images") / name;
        if (fs::exists(cand1)) return cand1.string();

        // Candidate 2: eng_images/eng_<name>
        fs::path cand2 = fs::path("eng_images") / (std::string("eng_") + name);
        if (fs::exists(cand2)) return cand2.string();

        // Candidate 3: try original path unchanged
        fs::path origp(originalPath);
        if (fs::exists(origp)) return originalPath;
    }
    return originalPath;
}

std::string ThreadStateControl::GetLocalizedVideoPath(const std::string &originalPath)
{
    namespace fs = std::filesystem;
    fs::path p(originalPath);
    std::string filename = p.filename().string();

    // Determine base filename without "Eng_" or "eng_" prefix if present
    std::string baseName = filename;
    if (baseName.rfind("Eng_", 0) == 0) {
        baseName = baseName.substr(4);
    } else if (baseName.rfind("eng_", 0) == 0) {
        baseName = baseName.substr(4);
    }

    if (msetting.Language == "English") {
        // Priority list for English video:
        // 1. Eng_videos/Eng_<baseName>
        // 2. Eng_videos/eng_<baseName>
        // 3. Eng_videos/<baseName>
        // 4. eng_videos/Eng_<baseName>
        // 5. eng_videos/eng_<baseName>
        // 6. eng_videos/<baseName>
        // 7. Videos/Eng_<baseName>
        // 8. Videos/eng_<baseName>
        // 9. originalPath (if exists)
        // 10. Videos/<baseName>
        std::vector<fs::path> candidates = {
            fs::path("Eng_videos") / ("Eng_" + baseName),
            fs::path("Eng_videos") / ("eng_" + baseName),
            fs::path("Eng_videos") / baseName,
            fs::path("eng_videos") / ("Eng_" + baseName),
            fs::path("eng_videos") / ("eng_" + baseName),
            fs::path("eng_videos") / baseName,
            fs::path("Videos") / ("Eng_" + baseName),
            fs::path("Videos") / ("eng_" + baseName),
            fs::path(originalPath),
            fs::path("Videos") / baseName
        };

        for (const auto& cand : candidates) {
            if (!cand.empty() && fs::exists(cand)) {
                return cand.string();
            }
        }
    } else {
        // Chinese or other languages:
        // Priority list for Chinese / default video:
        // 1. Videos/<baseName>
        // 2. videos/<baseName>
        // 3. originalPath (if exists)
        std::vector<fs::path> candidates = {
            fs::path("Videos") / baseName,
            fs::path("videos") / baseName,
            fs::path(originalPath)
        };

        for (const auto& cand : candidates) {
            if (!cand.empty() && fs::exists(cand)) {
                return cand.string();
            }
        }
    }

    return originalPath;
}

void ThreadStateControl::NextState()
{
    m_iStateIndex++;
}

void ThreadStateControl::run()
{
    chrono::time_point<chrono::system_clock> current_time;

    //wait until Kebbi is connected.
    mutex mtx;
    unique_lock<mutex> lk(mtx);
    cond_var_state_control.wait(lk);
    chrono::milliseconds tolerance_duration(1000);      //tolerance for onTTSComplete and patient's tSpeechStart
    chrono::seconds dance_wait_duration;
    chrono::time_point<chrono::system_clock> dance_start_time;

    //initialize the random seed.
    srand(time(0));
    bool bAccumulateConversation = false;
    string str_inserted_assistant_message;
    string str_assistant_message;
    string str_voice_source = "Patient"; //or "Video"
    while(b_WhileLoop)
    {
        current_time = chrono::system_clock::now();

        if(mStates[m_iStateIndex].bInitial)
        {
            cout << "Enter state " << m_iStateIndex << endl;
            if (mbStopInterrupted) {
                cout << "Stop interrupt active; suppressing state initialization speech." << endl;
                mStates[m_iStateIndex].bInitial = false;
                mbReadyToChangeState = false;
                mbOldStateComplete = false;
                continue;
            }
            if( mStates[m_iStateIndex].sImageFileName != "" )
            {
                std::string img = GetLocalizedImagePath(mStates[m_iStateIndex].sImageFileName);
                cout << "[ThreadStateControl] Language=" << msetting.Language << " selected image: " << img << endl;
                emit playImageRequest(img.c_str());
            }

            for( string s : mStates[m_iStateIndex].v_str_Action)
            {
                if( s.find("PlayVideo:") != string::npos )
                {
                    size_t pos = s.find(":");
                    std::string rawVideoPath = s.substr(pos + 1);
                    std::string locVideoPath = GetLocalizedVideoPath(rawVideoPath);
                    cout << "[ThreadStateControl] Language=" << msetting.Language << " selected video: " << locVideoPath << endl;
                    emit playVideoRequest(locVideoPath.c_str());
                    str_voice_source = "Video";
                }

                if( s == "KeepSilent" )
                {
                    //Do not send any message for a period of time
                    mbKeepSilent = true;
                }

                if( s == "KeepSilentOff" )
                {
                    //Do not send any message for a period of time
                    mbKeepSilent = false;
                }

                if( s == "AccumulateConversation" )
                {
                    //Do not clear the message history when entering this state.
                    mStates[m_iStateIndex].message_history = mStates[m_iStateIndex-1].message_history;
                    bAccumulateConversation = true;
                }

                if( s.find("InsertAssitantMessage:") != string::npos )
                {
                    size_t pos = s.find(":");
                    str_inserted_assistant_message = s.substr(pos + 1);
                }
            }
            
            mStates[m_iStateIndex].bInitial = false;
            mStates[m_iStateIndex].m_Start_time = chrono::system_clock::now();
            if( mStates[m_iStateIndex].m_strFirstSentence != "")
            {
                string sReplacedFirstSentence = ReplaceVariables(mStates[m_iStateIndex].m_strFirstSentence);
                str_assistant_message = sReplacedFirstSentence + str_inserted_assistant_message;
                str_inserted_assistant_message = "";   //clear the string
                mStates[m_iStateIndex].message_history.push_back("Assistant: "+str_assistant_message);
                RobotCommandProtobuf::RobotCommand command;
                command.set_speak_sentence(sReplacedFirstSentence);
                command.set_language(msetting.Language);
                command.set_sface(mStates[m_iStateIndex].sFace);
                if(mStates[m_iStateIndex].sMotion != "")
                {
                    command.set_smotion(mStates[m_iStateIndex].sMotion);
                    if( KebbiMoveHeadDuringMotion(command.smotion()) )
                    {
                        mpThreadProcessImage->NotifyEvent("KebbiMoveHeadDuringMotion", chrono::system_clock::now()); //pause watching patient
                    }
                }
                // BARGE-IN: robot starts speaking (TTS just queued).
                robotSpeaking.store(true, std::memory_order_relaxed);

                m_pSendMessageManager->AddMessage(command);
                mbTTSComplete = false;


                //just for display on UI
                mpThreadLLM->strResponse = sReplacedFirstSentence;
                mpThreadLLM->b_new_LLM_response = true;
            }
            mbWaitForTTSComplete = mStates[m_iStateIndex].bWaitForTTSComplete;      //now it is always true
            mbOldStateComplete = false;              //In the initial state, the old state is not complete.
            mbReadyToChangeState = false;

        }  //if(mStates[m_iStateIndex].bInitial)

        if( mStates[m_iStateIndex].sStateType == "SaySomething" )   //Robot does not wait for patient's response
        {
            if( current_time - mStates[m_iStateIndex].m_Start_time > mStates[m_iStateIndex].m_secDurationLimit)
            {
                mbReadyToChangeState = true;
                mbOldStateComplete = true;
            }
        }
        else if( mStates[m_iStateIndex].sStateType == "PlayVideo" )
        {
            // For PlayVideo state, wait until onVideoComplete event is received from VideoWindow (when media reaches EndOfMedia).
            // A generous fallback timeout (600s) prevents getting stuck if video window is closed or unavailable.
            auto maxDuration = std::max(mStates[m_iStateIndex].m_secDurationLimit, std::chrono::seconds(600));
            if( current_time - mStates[m_iStateIndex].m_Start_time > maxDuration )
            {
                cout << "[ThreadStateControl] Video safety timeout reached (" << maxDuration.count() << "s). Advancing state." << endl;
                mbReadyToChangeState = true;
                mbOldStateComplete = true;
            }
        }
        else if( mStates[m_iStateIndex].sStateType == "Conversation" || mStates[m_iStateIndex].sStateType == "RobotAskPatientAnswer")
        {
            if( mbWaitForTTSComplete)
            {
                if( mbTTSComplete)
                {
                    WhisperData WhisperResult = mpThreadWhisper->getLatestResult();
                    if( mStates[m_iStateIndex].m_strStateName == "Ask dance")
                    {
                        if( mStates[m_iStateIndex].iStage == 0 )  //Conversation
                        {
                            if( current_time - mStates[m_iStateIndex].m_Start_time > mStates[m_iStateIndex].m_secDurationLimit)
                            {
                                cout << "Time out, choose Egyptian dance." << endl;
                                chosen_dance = 1;
                                dance_wait_duration = chrono::seconds(73);
                                cout << "CHOSEN_DANCE: " << chosen_dance << "\n"; 
                            }
                            else        //string comparison
                            {
                                if(WhisperResult.sOutput.find("及") != string::npos || WhisperResult.sOutput.find("吉") != string::npos || WhisperResult.sOutput.find("極") != string::npos || WhisperResult.sOutput.find("級") != string::npos)
                                {
                                    chosen_dance = 1;
                                    dance_wait_duration = chrono::seconds(73);
                                }
                                else if (WhisperResult.sOutput.find("牛") != string::npos || WhisperResult.sOutput.find("仔") != string::npos )
                                {
                                    chosen_dance = 2;
                                    dance_wait_duration = chrono::seconds(81);
                                }
                            }

                            if( chosen_dance != 0 )
                            {
                                //debug
                                //cout << "(J) chosen_dance " << chosen_dance << endl;
                                RobotCommandProtobuf::RobotCommand dance_command;
                                dance_command.set_dancetype(chosen_dance);
                                m_pSendMessageManager->AddMessage(dance_command);
                                mStates[m_iStateIndex].iStage = 1; //Waiting for Dance Complete
                                dance_start_time = chrono::system_clock::now();
                            }
                        }
                        else if( mStates[m_iStateIndex].iStage == 1 )  //Wait for dance completion
                        {
                            //Use signal to control the state flow
                            //debug 
                            //cout << "(K) wait for mbActivity_mbtx_Complete as true" << endl;
                            if( mbActivity_mbtx_Complete )
                            {
                                mbOldStateComplete = true;
                                mbReadyToChangeState = true;

                                //turn of the face because the dance completes.
                                RobotCommandProtobuf::RobotCommand command;
                                command.set_hideface(0);
                                m_pSendMessageManager->AddMessage(command);
                            }
                        }
                    }
                    //There are two cases I accept a patient's response
                    //1. The patient's tSpeechStart is within the tolerance_duration before the mtimestamp_TTSComplete 
                    //2. The patient's tSpeechEnd is after the mtimestamp_TTSComplete, and current_time is a few seconds after tSpeechEnd
                    //2026/1/7 New case: the voice is from video playback rather than patient's response.
                    //That is the case I need to distinguish the voice source.
                    //Can I use any visual cue to help me determine whether the voice is from patient or video? It is very hard.
                    else if( WhisperResult.tSpeechStart > mtimestamp_TTSComplete - tolerance_duration || 
                            (WhisperResult.tSpeechEnd > mtimestamp_TTSComplete && current_time - WhisperResult.tSpeechEnd > 3s) )
                    {
                        //debug
                        cout << "Patient's response: " << WhisperResult.sOutput << endl;

                        if (msetting.bFilterHallucinatedNoiseText && ThreadWhisper::IsPureNoiseOrHallucination(WhisperResult.sOutput))
                        {
                            cout << "[ThreadStateControl] Discarding ambient noise/hallucination response: '" << WhisperResult.sOutput << "'" << endl;
                            continue;
                        }

                        // ---------------------------------------------------------
                        // Voice interruption (STOP keyword)
                        // ---------------------------------------------------------
                        std::string patientSpeech = WhisperResult.sOutput;

                        // convert to lowercase
                        std::transform(patientSpeech.begin(),
                                       patientSpeech.end(),
                                       patientSpeech.begin(),
                                       ::tolower);

                        if (patientSpeech == "stop" ||
                            patientSpeech == "stop." ||
                            patientSpeech == "stop talking" ||
                            patientSpeech == "stop talking." ||
                            patientSpeech == "be quiet" ||
                            patientSpeech == "cancel")
                        {
                            std::cout << "[STOP] Stop keyword detected." << std::endl;

                            RobotCommandProtobuf::RobotCommand command;

                            // Stop Kebbi speech immediately
                            command.set_stoptts(true);

                            m_pSendMessageManager->AddMessage(command);

                            // State management: freeze current interaction until a future resume/continue.
                            mbTTSComplete = false;
                            mbWaitForTTSComplete = true;
                            mbOldStateComplete = false;
                            mbReadyToChangeState = false;

                            std::cout << "[STOP] Stop command sent to robot." << std::endl;

                            continue;
                        }


                        //get patient's name
                        if( mStates[m_iStateIndex].m_strStateName == "Greeting 1 and ask name" )
                        {
                            msPatientName = GetPatientName(WhisperResult.sOutput);
                            cout << "Extracted patient name: " << msPatientName << endl;
                        }


                        if( mStates[m_iStateIndex].v_str_KeyWordMoveToNextState.size() > 0)
                        {
                            for( size_t k = 0; k < mStates[m_iStateIndex].v_str_KeyWordMoveToNextState.size(); k++)
                            {
                                if( WhisperResult.sOutput.find( mStates[m_iStateIndex].v_str_KeyWordMoveToNextState.at(k) ) != string::npos)
                                {
                                    mbReadyToChangeState = true;
                                    mbOldStateComplete = true;
                                    break;
                                }
                            }
                        }

                        mStates[m_iStateIndex].message_history.push_back("User: "+WhisperResult.sOutput);

                        if (mStates[m_iStateIndex].sStateType == "RobotAskPatientAnswer")
                        {
                            mbReadyToChangeState = true;   //After the patient answers the question, the robot can move to the next state without waiting for LLM response.
                        }

                        if(mbReadyToChangeState )
                        {
                            mbOldStateComplete = true;
                        }
                        else if (mStates[m_iStateIndex].sStateType == "Conversation")
                        {
                            //generate LLM response
                            //debug
                            DumpHistoryMessages(mStates[m_iStateIndex].message_history);
                            //generate LLM result;
                            mbWaitForTTSComplete = false;
                            mbWaitForLLMResult = true;
                            LLMTask task;
                            task.str_message = ConvertMessageHistoryToString( mStates[m_iStateIndex].message_history );
                            task.timestamp = chrono::system_clock::now();
                            task.bNotify = true;
                            mpThreadLLM->AddQueue(task);
                            mpThreadLLM->cond_var_thread_LLM.notify_one();
                        }
                    }
                }
            }   //if( mbWaitForTTSComplete)

            if( mbWaitForLLMResult)
            {
                if( mbLLMResult)
                {
                    //Ollama.hpp's format is different from AnythingLLM.
                    //Ollama.hpp does not have the "Assistant: " prefix in the response, while AnythingLLM has.
//                    mStates[m_iStateIndex].message_history.push_back("Assistant: "+msLLMResult);
                    mStates[m_iStateIndex].message_history.push_back(msLLMResult);
                    
                    //Here is the command to let Kebbi speak the LLM result
                    if( !mbKeepSilent)
                    {
                        RobotCommandProtobuf::RobotCommand command;
                        size_t pos = msLLMResult.find("Assistant: ");
                        string str_assistant_message;
                        if( pos != string::npos )
                        {
                            str_assistant_message = msLLMResult.substr(pos + string("Assistant: ").length());
                        }
                        else
                        {
                            str_assistant_message = msLLMResult;
                        }
                        command.set_speak_sentence(str_assistant_message);
                        command.set_language(msetting.Language);
                        //randomly choose a motion
                        if( mStates[m_iStateIndex].vSmallMotion.size() > 0)
                        {
                            int randomNumber = (rand() % mStates[m_iStateIndex].vSmallMotion.size());
                            command.set_smotion(mStates[m_iStateIndex].vSmallMotion.at(randomNumber));
                        }

                        m_pSendMessageManager->AddMessage(command);
                        mbTTSComplete = false;
                        mbWaitForTTSComplete = true;
                        mbWaitForLLMResult = false;
                        mbLLMResult = false;
                    }
                }
            }

            //Check if the time exceed the state limit
            if(current_time - mStates[m_iStateIndex].m_Start_time > mStates[m_iStateIndex].m_secDurationLimit)
            {
                mbReadyToChangeState = true;
            }
        }
        else //neither StateAndListen nor Conversation
        {
            //unknown state type
            cout << "Unknown state type: " << mStates[m_iStateIndex].sStateType << endl;
        }

        if( mbReadyToChangeState && mbOldStateComplete)
        {
            if(mStates[m_iStateIndex].bEndState)
            {
                b_WhileLoop = false;
            }
            else
            {
                m_iStateIndex = mStates[m_iStateIndex].iNextStateIndex;
                mbReadyToChangeState = false;
            }
        }

        this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ThreadStateControl::NotifyEvent(string description, chrono::time_point<chrono::system_clock> timestamp, string sLLMResult)
{
    if( description == "onTTSComplete")
    {
        // BARGE-IN: robot finished speaking.
        robotSpeaking.store(false, std::memory_order_relaxed);
        stopRequested.store(false, std::memory_order_relaxed);

        mbTTSComplete = true;
        mtimestamp_TTSComplete = timestamp;

        //debug
        //cout << "(E)" << endl;
        //cout << "NotifyEvent mtimestamp_TTSComplete " << ConvertTimeToString(mtimestamp_TTSComplete) << endl;
    }
    else if( description == "onLLMResult")
    {
        mbLLMResult = true;
        mtimestamp_LLMResult = timestamp;
        msLLMResult = sLLMResult;
        //debug
        //cout << "(H)" << endl;
        //cout << "NotifyEvent onLLMResult" << endl;
    }
    else if( description == "onActivityRestart")
    {
        mbActivity_mbtx_Complete = true;
        mtimestamp_Activity_mbtx_Complete = timestamp;
    }
    else if( description == "onVideoComplete")
    {
        //debug
        cout << "NotifyEvent onVideoComplete" << endl;
        mbReadyToChangeState = true;
        mbOldStateComplete = true;
        mtimestamp_VideoComplete = timestamp;     //In fact, it is not TTSComplete, but I reuse this variable to store the time when video is complete.
    }
}

//N is the initial state index
void ThreadStateControl::SetIntialStateIndex(int N) { 
    if( N < 0 || N >= m_iNumberOfStates)
    throw "Invalid stage number: " + std::to_string(N);
    m_iStateIndex = N; 
}


string ThreadStateControl::GetPatientName(string input_sentence){
    ollama::options options;
    options["seed"] = rand();
    options["temperature"] = 0.1;
    options["num_ctx"] = 131072;

    string prompt = "You are a robot assistant named Kebbi. Extract ONLY the speaker's (patient's) name from the input sentence.\n"
                    "Rules:\n"
                    "1. Return ONLY the exact speaker name present in the input sentence.\n"
                    "2. Do NOT include the robot's name (Kebbi, Kebby, KV, 凱比, 凯比).\n"
                    "3. Do NOT include greetings (hello, hi, I am, my name is) or explanations.\n"
                    "4. Do NOT hallucinate or invent names not spoken in the input sentence.\n"
                    "Sentence: \"" + input_sentence + "\"\n"
                    "Speaker Name:";    

    string ModelName = "gemma3:1b";
    string name = ollama::generate(ModelName, prompt, options);

    // Robot name patterns to strip out case-insensitively
    vector<string> robot_names = {"kebbi", "kebby", "kv", "zenbo", "凱比", "凯比"};
    
    stringstream ss(name);
    string line;
    string cleaned_name = "";
    
    while (getline(ss, line)) {
        // Trim whitespace and quotes
        auto start = line.find_first_not_of(" \t\r\n\"'");
        if (start == string::npos) continue;
        auto end = line.find_last_not_of(" \t\r\n\"'.");
        line = line.substr(start, end - start + 1);
        if (line.empty()) continue;

        string line_lower = line;
        transform(line_lower.begin(), line_lower.end(), line_lower.begin(), ::tolower);
        
        bool is_robot_name = false;
        for (const auto& rname : robot_names) {
            if (line_lower == rname) {
                is_robot_name = true;
                break;
            }
        }
        if (is_robot_name) continue;

        // Strip embedded robot names
        for (const auto& rname : robot_names) {
            size_t pos;
            while ((pos = line_lower.find(rname)) != string::npos) {
                line.erase(pos, rname.length());
                line_lower.erase(pos, rname.length());
            }
        }

        // Re-trim line after removing robot name
        start = line.find_first_not_of(" \t\r\n\"',.");
        if (start == string::npos) continue;
        end = line.find_last_not_of(" \t\r\n\"',.");
        line = line.substr(start, end - start + 1);

        if (!line.empty()) {
            if (!cleaned_name.empty()) cleaned_name += " ";
            cleaned_name += line;
        }
    }

    // Helper lambda to extract name from phrase if LLM hallucinates
    auto extract_name_from_phrase = [](const string& sentence) -> string {
        string lower = sentence;
        transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        vector<string> prefixes = {
            "my name is ", "i am ", "i'm ", "call me ", "this is ",
            "我是", "叫我", "我叫"
        };
        for (const auto& pref : prefixes) {
            size_t pos = lower.find(pref);
            if (pos != string::npos) {
                string sub = sentence.substr(pos + pref.length());
                auto start = sub.find_first_not_of(" \t\r\n\"',.");
                if (start != string::npos) {
                    auto end = sub.find_last_not_of(" \t\r\n\"',.");
                    sub = sub.substr(start, end - start + 1);
                    if (!sub.empty()) return sub;
                }
            }
        }
        return "";
    };

    // Sanity check against input_sentence: check if words in cleaned_name exist in input_sentence
    string input_lower = input_sentence;
    transform(input_lower.begin(), input_lower.end(), input_lower.begin(), ::tolower);
    string cleaned_lower = cleaned_name;
    transform(cleaned_lower.begin(), cleaned_lower.end(), cleaned_lower.begin(), ::tolower);

    stringstream token_ss(cleaned_lower);
    string word;
    int total_words = 0;
    int invalid_words = 0;
    while (token_ss >> word) {
        total_words++;
        if (input_lower.find(word) == string::npos) {
            invalid_words++;
        }
    }

    if (total_words == 0 || invalid_words > 0) {
        string fallback = extract_name_from_phrase(input_sentence);
        if (!fallback.empty()) {
            cleaned_name = fallback;
        } else if (cleaned_name.empty()) {
            auto start_pos = name.find_first_not_of(" \n\r\t\"'");
            if (start_pos != string::npos) {
                auto end_pos = name.find_last_not_of(" \n\r\t\"'.");
                cleaned_name = name.substr(start_pos, end_pos - start_pos + 1);
            }
        }
    }

    cout << "Extracted patient name by LLM (raw): " << name << " -> cleaned: " << cleaned_name << endl;
    
    return cleaned_name;
}

string ThreadStateControl::ConvertMessageHistoryToString(vector<string> message_history)
{
    string result = "";
    for( const auto& message : message_history)
    {
        result += message + "\n";
    }
    return result;
}

void ThreadStateControl::DumpHistoryMessages(vector<string> messages)
{
    cout << "---- Dumping message history ----" << endl;
    for( const auto& message : messages)
    {
        cout << message << endl;
    }
    cout << "---- End of message history ----" << endl;
}

string ThreadStateControl::ReplaceVariables(string sentence)
{
    //This function is used to replace the variables in the sentence with the real values. For example, replace {PatientName} with the real patient name.
    string result = sentence;
    if( result.find("{PatientName}") != string::npos)
    {
        result.replace(result.find("{PatientName}"), string("{PatientName}").length(), msPatientName);
    }

    if( result.find("{PatientTitle}") != string::npos)
    {
        string sPatientGender = mpThreadProcessImage->GetPatientGender(); //update the patient
        int iPatientAge = mpThreadProcessImage->GetPatientAge();

        if (sPatientGender == "Male")
        {
            msPatientTitle = m_mapPatientTitles["MaleAdult"];
            if (msPatientTitle.empty()) {
                msPatientTitle = (msetting.Language == "English") ? "Mr." : "先生";
            }
        }
        else if (sPatientGender == "Female")
        {
            if (iPatientAge != -1 && iPatientAge < 40 && m_mapPatientTitles.count("FemaleYoungAdult") && !m_mapPatientTitles["FemaleYoungAdult"].empty())
            {
                msPatientTitle = m_mapPatientTitles["FemaleYoungAdult"];
            }
            else if (m_mapPatientTitles.count("FemaleOlderAdult") && !m_mapPatientTitles["FemaleOlderAdult"].empty())
            {
                msPatientTitle = m_mapPatientTitles["FemaleOlderAdult"];
            }
            else
            {
                msPatientTitle = (msetting.Language == "English") ? "Ms." : "女士";
            }
        }
        else
        {
            if (iPatientAge != -1 && iPatientAge < 10 && m_mapPatientTitles.count("Child") && !m_mapPatientTitles["Child"].empty())
            {
                msPatientTitle = m_mapPatientTitles["Child"];
            }
            else
            {
                msPatientTitle = (msetting.Language == "English") ? "Mr." : "先生";
            }
        }

        result.replace(result.find("{PatientTitle}"), string("{PatientTitle}").length(), msPatientTitle);
    }
    return result;
}

void ThreadStateControl::SetStopInterrupted(bool interrupted)
{
    mbStopInterrupted = interrupted;
}

void ThreadStateControl::Restart()
{
    m_iStateIndex = 0;
    mbStopInterrupted = true;
    for( auto& state : mStates)
    {
        state.bInitial = true;
        state.bWaitForTTSComplete = true;
        state.bEndState = false;
    }
}

void ThreadStateControl::LoadPatientTitles()
{
    m_mapPatientTitles.clear();

    // Set fallback defaults based on language
    if (msetting.Language == "English") {
        m_mapPatientTitles["Child"] = "Kid";
        m_mapPatientTitles["Youth"] = "";
        m_mapPatientTitles["MaleAdult"] = "Mr.";
        m_mapPatientTitles["FemaleYoungAdult"] = "Miss";
        m_mapPatientTitles["FemaleOlderAdult"] = "Ms.";
    } else {
        // Default to Chinese
        m_mapPatientTitles["Child"] = "小朋友";
        m_mapPatientTitles["Youth"] = "";
        m_mapPatientTitles["MaleAdult"] = "先生";
        m_mapPatientTitles["FemaleYoungAdult"] = "小姐";
        m_mapPatientTitles["FemaleOlderAdult"] = "女士";
    }

    // Try to load from dynamic text file
    string fileName = "PatientTitle_" + msetting.Language + ".txt";
    ifstream file(fileName);
    if (!file.is_open()) {
        if (msetting.Language == "Arabic") {
            fileName = "PatientTitle_English.txt";
            file.open(fileName);
        }
    }

    if (file.is_open()) {
        string line;
        while (getline(file, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            if (line.empty() || line[0] == '#') continue;

            size_t sep = line.find(':');
            if (sep != string::npos) {
                string key = line.substr(0, sep);
                string value = line.substr(sep + 1);

                // Trim key and value
                key.erase(0, key.find_first_not_of(" \t\r\n"));
                key.erase(key.find_last_not_of(" \t\r\n") + 1);
                value.erase(0, value.find_first_not_of(" \t\r\n"));
                value.erase(value.find_last_not_of(" \t\r\n") + 1);

                m_mapPatientTitles[key] = value;
            }
        }
        file.close();
    } else {
        cout << "Warning: Could not open patient title text file: " << fileName << ". Using defaults." << endl;
    }
}
