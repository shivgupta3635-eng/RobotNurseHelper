#include "ThreadLLM.hpp"
#include "utility_string.hpp"

ThreadLLM::ThreadLLM()
{
//    LoadJSONFile(msetting, "json/Setting.json");
}

ThreadLLM::~ThreadLLM()
{
}

void ThreadLLM::run()
{
    mutex mtx;
    unique_lock<mutex> lk(mtx);
    while(b_WhileLoop)
    {
        cond_var_thread_LLM.wait(lk);

        if(mqueue.size() > 0)
        {
            LLMTask task = mqueue.front();
            mqueue.pop();

            AnythingLLM anythingLLM("127.0.0.1", 3001, msetting.AnythingLLM_API_key);
            strResponse = anythingLLM.ask(msetting.AnythingLLM_workspace_slug, task.str_message);

            b_new_LLM_response = true;
            if( task.bNotify)
                mpThreadStateControl->NotifyEvent("onLLMResult", chrono::system_clock::now() ,strResponse);
        }
    }
    cout << "Exit thread LLM while loop." << endl;
}

void ThreadLLM::AddQueue(LLMTask task)
{
    std::string trimmed = task.str_message;
    auto start = trimmed.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return;
    auto end = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(start, end - start + 1);

    if (trimmed.empty()) return;

    mqueue.push(task);
}

void ThreadLLM::SetSettingFile(const QString &filePath)
{
    LoadJSONFile(msetting, filePath.toStdString());
}