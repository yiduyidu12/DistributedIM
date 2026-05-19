#include "MessageHandler.h"

void MessageHandler::registerHandler(const std::string& type, HandlerFunc func) {
    handlers_[type] = func;
}

std::string MessageHandler::handle(const std::string& type, const std::string& msg, const std::string& username) {
    auto it = handlers_.find(type);
    if (it != handlers_.end()) {
        return it->second(msg, username);
    }
    return "{\"type\":\"error\",\"msg\":\"unknown type\"}\n";
}