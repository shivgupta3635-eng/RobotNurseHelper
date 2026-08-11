#include "ThreadStateControl.hpp"
#include <fstream>
#include "utility_time.hpp"

#include "BargeInControl.hpp"


#include <cstdlib> // For rand() and srand()
#include <ctime>   // For time()
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

    std::string baseName = filename;
    if (baseName.rfind("Eng_", 0) == 0) {
        baseName = baseName.substr(4);
    } else if (baseName.rfind("eng_", 0) == 0) {
        baseName = baseName.substr(4);
    }

    if (msetting.Language == "English") {
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
                    std::string localizedVideoPath = GetLocalizedVideoPath(rawVideoPath);
                    cout << "[ThreadStateControl] Language=" << msetting.Language << " selected video: " << localizedVideoPath << endl;
                    emit playVideoRequest(localizedVideoPath.c_str());
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

        bool bQuestionSaySomethingState =
            (mStates[m_iStateIndex].sStateType == "SaySomething" &&
             mStates[m_iStateIndex].v_str_KeyWordMoveToNextState.size() > 0);

        bool bConversationResponseState =
            mStates[m_iStateIndex].sStateType == "Conversation" ||
            mStates[m_iStateIndex].sStateType == "RobotAskPatientAnswer" ||
            bQuestionSaySomethingState;

        if( bConversationResponseState )
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
                            std::string extractedName = GetPatientName(WhisperResult.sOutput);
                            if( !extractedName.empty() )
                            {
                                msPatientName = extractedName;
                                cout << "Extracted patient name: " << msPatientName << endl;
                                mbReadyToChangeState = true;
                                mbOldStateComplete = true;
                            }
                            else
                            {
                                cout << "Name not recognized for state 'Greeting 1 and ask name': " << WhisperResult.sOutput << endl;
                                // Do not let the greeting question become a free-form LLM conversation.
                                // Keep the state alive and wait for a name-bearing answer.
                                mbReadyToChangeState = false;
                                mbOldStateComplete = false;
                            }
                        }

                        // A fresh Whisper result is processed exactly once here.
                        // Clear the latest result so the next control-loop iteration does not
                        // feed the same STT transcript back as a new patient answer.
                        mpThreadWhisper->ClearLatestResult();

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

            //Check if the time exceed the state limit. For Conversation and RobotAskPatientAnswer,
            //a timeout must also count as a completed state or the loop can stall indefinitely.
            if(current_time - mStates[m_iStateIndex].m_Start_time > mStates[m_iStateIndex].m_secDurationLimit)
            {
                mbReadyToChangeState = true;
                if( mStates[m_iStateIndex].sStateType == "Conversation" ||
                    mStates[m_iStateIndex].sStateType == "RobotAskPatientAnswer")
                {
                    mbOldStateComplete = true;
                }
            }
        }
        else
        {
            if( mStates[m_iStateIndex].sStateType == "SaySomething" || mStates[m_iStateIndex].sStateType == "PlayVideo")
            {
                if( current_time - mStates[m_iStateIndex].m_Start_time > mStates[m_iStateIndex].m_secDurationLimit)
                {
                    mbReadyToChangeState = true;
                    mbOldStateComplete = true;
                }
            }
            else
            {
                //unknown state type
                cout << "Unknown state type: " << mStates[m_iStateIndex].sStateType << endl;
            }
        }

        if( mbReadyToChangeState && mbOldStateComplete)
        {
            if(mStates[m_iStateIndex].bEndState)
            {
                b_WhileLoop = false;
            }
            else
            {
                int nextStateIndex = mStates[m_iStateIndex].iNextStateIndex;
                if( nextStateIndex < 0 || nextStateIndex >= static_cast<int>(mStates.size()) )
                {
                    cout << "[ThreadStateControl] Invalid next state index " << nextStateIndex
                         << " for state " << m_iStateIndex << ", stopping the state loop." << endl;
                    b_WhileLoop = false;
                    continue;
                }
                m_iStateIndex = nextStateIndex;
                mbReadyToChangeState = false;
                mbOldStateComplete = false;
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


string ThreadStateControl::GetPatientName(string input_sentence)
{
    std::string lower = input_sentence;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    std::string candidate;
    bool namePhraseFound = false;

    // Check English and Chinese name intro phrases.
    vector<string> prefixes = {
        "you can call me ", "my name is ", "i am ", "i'm ", "im ",
        "this is ", "call me ", "name is ", "i go by ",
        "您可以叫我", "你可以叫我", "我叫", "叫我", "我是", "稱呼我", "称呼我"
    };

    for (const auto& pref : prefixes)
    {
        size_t pos = lower.find(pref);
        if (pos != std::string::npos)
        {
            candidate = input_sentence.substr(pos + pref.length());
            namePhraseFound = true;
            break;
        }
    }

    if (!namePhraseFound)
    {
        // If no explicit prefix was spoken, check if input_sentence itself is a clean direct name response.
        string trimmed = input_sentence;
        trimmed.erase(0, trimmed.find_first_not_of(" \n\r\t"));
        size_t last = trimmed.find_last_not_of(" \n\r\t");
        if (last != string::npos) trimmed = trimmed.substr(0, last + 1);

        while (!trimmed.empty() && (trimmed.back() == '.' || trimmed.back() == ',' || trimmed.back() == '!' || trimmed.back() == '?' || trimmed.back() == '"' || trimmed.back() == '\''))
        {
            trimmed.pop_back();
        }

        string lowerTrimmed = trimmed;
        std::transform(lowerTrimmed.begin(), lowerTrimmed.end(), lowerTrimmed.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

        vector<string> stopWords = {
            "yes", "yeah", "no", "okay", "ok", "ready", "start", "begin", "hello", "hi",
            "what", "sorry", "pardon", "repeat", "huh", "don't know", "dont know",
            "好", "不可以", "不能", "不要", "開始", "介绍", "介紹", "不知道"
        };

        bool isStopWord = false;
        for (const auto& w : stopWords)
        {
            if (lowerTrimmed == w)
            {
                isStopWord = true;
                break;
            }
        }

        if (!isStopWord && !trimmed.empty())
        {
            candidate = trimmed;
            namePhraseFound = true;
        }
    }

    if (!namePhraseFound)
    {
        cout << "No name phrase or valid name response found in transcript for: " << input_sentence << endl;
        return "";
    }

    // Clean the candidate.
    candidate.erase(0, candidate.find_first_not_of(" \n\r\t"));
    size_t end = candidate.find_last_not_of(" \n\r\t");
    if (end != string::npos)
    {
        candidate = candidate.substr(0, end + 1);
    }

    while (!candidate.empty() &&
           (candidate.back() == '.' || candidate.back() == ',' || candidate.back() == '!' || candidate.back() == '?' || candidate.back() == '"' || candidate.back() == '\''))
    {
        candidate.pop_back();
    }

    std::string lowerCandidate = candidate;
    std::transform(lowerCandidate.begin(), lowerCandidate.end(), lowerCandidate.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    if (lowerCandidate == "yes" || lowerCandidate == "yeah" || lowerCandidate == "no" || lowerCandidate == "okay" || lowerCandidate == "ok" || lowerCandidate == "ready" || lowerCandidate == "start" || lowerCandidate == "begin" || lowerCandidate == "hello" || lowerCandidate == "hi" || lowerCandidate == "what" || lowerCandidate == "sorry")
    {
        cout << "Rejecting non-name transcript token as patient name: " << candidate << endl;
        return "";
    }

    string extractedName = candidate;
    if (extractedName.empty())
    {
        // LLM fallback with language-aware prompt
        ollama::options options;
        options["seed"] = rand();
        options["temperature"] = 0.3;
        options["num_ctx"] = 131072;

        string prompt;
        if (msetting.Language == "English")
        {
            prompt = "You are a robot assistant. Please extract the speaker's name from the following sentence. "
                     "Rules: 1. Output ONLY the name. 2. Do not include any punctuation or extra explanation. "
                     "Sentence: \"" + input_sentence + "\"";
        }
        else
        {
            prompt = "你是一個機器人助手。請從以下句子中提取說話者的姓名。"
                     "規則：1.只回傳姓名 2.不要有任何標點或解釋。"
                     "句子：\"" + input_sentence + "\"";
        }
        string ModelName = "gemma3:1b";
        extractedName = ollama::generate(ModelName, prompt, options);
        extractedName.erase(0, extractedName.find_first_not_of(" \n\r\t"));
        if (extractedName.find_last_not_of(" \n\r\t") != string::npos)
            extractedName.erase(extractedName.find_last_not_of(" \n\r\t") + 1);
    }

    // Extract title (salutation) if present in spoken name
    string lowerName = extractedName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    if (lowerName.rfind("mr. ", 0) == 0 || lowerName.rfind("mr ", 0) == 0)
    {
        msPatientTitle = "Mr.";
        size_t sp = extractedName.find(' ');
        if (sp != string::npos) extractedName = extractedName.substr(sp + 1);
    }
    else if (lowerName.rfind("ms. ", 0) == 0 || lowerName.rfind("ms ", 0) == 0)
    {
        msPatientTitle = "Ms.";
        size_t sp = extractedName.find(' ');
        if (sp != string::npos) extractedName = extractedName.substr(sp + 1);
    }
    else if (lowerName.rfind("mrs. ", 0) == 0 || lowerName.rfind("mrs ", 0) == 0)
    {
        msPatientTitle = "Mrs.";
        size_t sp = extractedName.find(' ');
        if (sp != string::npos) extractedName = extractedName.substr(sp + 1);
    }
    else if (lowerName.rfind("miss ", 0) == 0)
    {
        msPatientTitle = "Miss";
        size_t sp = extractedName.find(' ');
        if (sp != string::npos) extractedName = extractedName.substr(sp + 1);
    }
    else if (lowerName.rfind("dr. ", 0) == 0 || lowerName.rfind("dr ", 0) == 0)
    {
        msPatientTitle = "Dr.";
        size_t sp = extractedName.find(' ');
        if (sp != string::npos) extractedName = extractedName.substr(sp + 1);
    }
    else if (extractedName.length() > 6 && extractedName.substr(extractedName.length() - 6) == "先生")
    {
        msPatientTitle = "先生";
        extractedName = extractedName.substr(0, extractedName.length() - 6);
    }
    else if (extractedName.length() > 6 && extractedName.substr(extractedName.length() - 6) == "小姐")
    {
        msPatientTitle = "小姐";
        extractedName = extractedName.substr(0, extractedName.length() - 6);
    }
    else if (extractedName.length() > 6 && extractedName.substr(extractedName.length() - 6) == "女士")
    {
        msPatientTitle = "女士";
        extractedName = extractedName.substr(0, extractedName.length() - 6);
    }

    extractedName.erase(0, extractedName.find_first_not_of(" \n\r\t"));
    size_t lastPos = extractedName.find_last_not_of(" \n\r\t");
    if (lastPos != string::npos) extractedName = extractedName.substr(0, lastPos + 1);

    cout << "Extracted patient name: " << extractedName << " (title: " << msPatientTitle << ")" << endl;
    return extractedName;
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

    // Ensure m_mapPatientTitles is populated
    if (m_mapPatientTitles.empty())
    {
        LoadPatientTitles();
    }

    // Determine or update msPatientTitle if not explicitly set from spoken input
    string sPatientGender = mpThreadProcessImage ? mpThreadProcessImage->GetPatientGender() : "Unknown";
    int iPatientAge = mpThreadProcessImage ? mpThreadProcessImage->GetPatientAge() : -1;

    if (msPatientTitle.empty())
    {
        if (iPatientAge != -1)
        {
            if (iPatientAge < 10)
            {
                msPatientTitle = m_mapPatientTitles["Child"];
            }
            else if (iPatientAge < 20)
            {
                msPatientTitle = m_mapPatientTitles["Youth"];
            }
            else
            {
                if (sPatientGender == "Male")
                {
                    msPatientTitle = m_mapPatientTitles["MaleAdult"];
                }
                else if (sPatientGender == "Female")
                {
                    if (iPatientAge < 40)
                    {
                        msPatientTitle = m_mapPatientTitles["FemaleYoungAdult"];
                    }
                    else
                    {
                        msPatientTitle = m_mapPatientTitles["FemaleOlderAdult"];
                    }
                }
            }
        }

        // If age is unknown (-1) or title is still empty, check gender
        if (msPatientTitle.empty())
        {
            if (sPatientGender == "Male")
            {
                msPatientTitle = m_mapPatientTitles["MaleAdult"];
            }
            else if (sPatientGender == "Female")
            {
                msPatientTitle = m_mapPatientTitles["FemaleOlderAdult"];
            }
        }

        // Fallback default if title is still empty when {PatientTitle} is requested
        if (msPatientTitle.empty())
        {
            if (msetting.Language == "English")
            {
                msPatientTitle = "Mr.";
            }
            else
            {
                msPatientTitle = "先生";
            }
        }
    }

    if (result.find("{PatientName}") != string::npos)
    {
        result.replace(result.find("{PatientName}"), string("{PatientName}").length(), msPatientName);
    }

    if (result.find("{PatientTitle}") != string::npos)
    {
        result.replace(result.find("{PatientTitle}"), string("{PatientTitle}").length(), msPatientTitle);
    }

    // Clean up double spaces resulting from empty title or variable replacement
    size_t doubleSpace = result.find("  ");
    while (doubleSpace != string::npos)
    {
        result.replace(doubleSpace, 2, " ");
        doubleSpace = result.find("  ");
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
    msPatientName = "";
    msPatientTitle = "";
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
        m_mapPatientTitles["Youth"] = "Student";
        m_mapPatientTitles["MaleAdult"] = "Mr.";
        m_mapPatientTitles["FemaleYoungAdult"] = "Miss";
        m_mapPatientTitles["FemaleOlderAdult"] = "Ms.";
    } else {
        // Default to Chinese
        m_mapPatientTitles["Child"] = "小朋友";
        m_mapPatientTitles["Youth"] = "同學";
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
