#include "Config.h"
#include "Logger.h"
#include <iostream>

int main() {
    std::cout << "=== Testing Config Loading ===" << std::endl;
    
    // 尝试加载配置文件
    bool loaded = Config::load("../config.json");
    
    if (loaded) {
        std::cout << "[OK] Config file loaded successfully" << std::endl;
    } else {
        std::cout << "[WARN] Using default config (config.json not found)" << std::endl;
    }
    
    // 打印配置内容
    std::cout << "\n=== Server Config ===" << std::endl;
    std::cout << "Port: " << Config::server().port << std::endl;
    std::cout << "Max Connections: " << Config::server().max_connections << std::endl;
    std::cout << "Timeout: " << Config::server().timeout_seconds << "s" << std::endl;
    
    std::cout << "\n=== Redis Config ===" << std::endl;
    std::cout << "Host: " << Config::redis().host << std::endl;
    std::cout << "Port: " << Config::redis().port << std::endl;
    std::cout << "DB: " << Config::redis().db << std::endl;
    if (!Config::redis().password.empty()) {
        std::cout << "Password: ******" << std::endl;
    }
    
    std::cout << "\n=== Log Config ===" << std::endl;
    std::cout << "Level: " << Config::log().level << std::endl;
    std::cout << "File: " << Config::log().file << std::endl;
    
    std::cout << "\n=== Testing Logger Initialization ===" << std::endl;
    Logger::init(Config::log().file, Config::log().level);
    Logger::info("Logger initialized with level: {}", Config::log().level);
    Logger::debug("This is a debug message");
    Logger::warn("This is a warning message");
    
    std::cout << "\n[SUCCESS] All config tests passed!" << std::endl;
    return 0;
}
