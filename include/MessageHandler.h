#pragma once

#include <string>
#include <unordered_map>
#include <functional>

class MessageHandler {
public:
    using HandlerFunc = std::function<std::string(const std::string&, const std::string&)>;

    void registerHandler(const std::string& type, HandlerFunc func);
    std::string handle(const std::string& type, const std::string& msg, const std::string& username);

private:
    std::unordered_map<std::string, HandlerFunc> handlers_;
};