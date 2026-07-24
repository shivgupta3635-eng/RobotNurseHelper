#include <QObject>

#include <memory>
#include <nlohmann/json.hpp>
#include <iostream>

#include <vector>
#include <string>
#include "AnythingLLM.hpp"

AnythingLLM::AnythingLLM(std::string host, int port, std::string api_key) 
    : key("Bearer " + api_key) {
    
    cli = std::make_unique<httplib::Client>(host, port);
    startNewPatient();
    cli->set_connection_timeout(5, 0); 
    cli->set_read_timeout(60, 0); 
}

std::string AnythingLLM::ask(std::string slug, std::string message) {
    nlohmann::json body = {
        {"message", message}, 
        {"mode", "chat"}, 
        {"sessionId", current_session}
    };
    
    httplib::Headers headers = {
        {"Authorization", key}, 
        {"Content-Type", "application/json"}
    };
    
    std::string path = "/api/v1/workspace/" + slug + "/chat";
    
    auto res = cli->Post(path.c_str(), headers, body.dump(), "application/json");
    
    if (res) {
        if (res->status == 200) {
            try {
                auto res_json = nlohmann::json::parse(res->body);
                if (res_json.contains("textResponse") && !res_json["textResponse"].is_null()) {
                    return res_json["textResponse"].get<std::string>();
                }
            } catch (const std::exception& e) {
                std::cerr << "[AnythingLLM] JSON parse error: " << e.what() << std::endl;
            }
        } else {
            std::cerr << "[AnythingLLM] HTTP Error " << res->status << " for workspace '" << slug << "': " << res->body << std::endl;
            return "";
        }
    }
    
    std::cerr << "[AnythingLLM] Connection to AnythingLLM (127.0.0.1:3001) failed. Please check if AnythingLLM service is running." << std::endl;
    return "";
}

void AnythingLLM::startNewPatient() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    current_session = "patient-" + std::to_string(now);
    std::cout << "[System] New conversation session ID: " << current_session << std::endl;
}    
