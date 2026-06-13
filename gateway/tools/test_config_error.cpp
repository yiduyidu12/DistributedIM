#include "Config.h"
#include <iostream>

int main() {
    std::cout << "=== Testing Config Error Handling ===\n" << std::endl;
    
    // 测试无效配置文件
    std::cout << "[Test 1] Loading invalid config file..." << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    bool result = Config::load("../test_bad_config.ini");
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Load result: " << (result ? "SUCCESS" : "FAILED") << std::endl;
    std::cout << std::endl;
    
    // 显示最终配置值（应该使用默认值）
    std::cout << "[Test 2] Final configuration values:" << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Server Port: " << Config::server().port << " (expected: 8888)" << std::endl;
    std::cout << "Max Connections: " << Config::server().max_connections << " (expected: 1024)" << std::endl;
    std::cout << "Timeout: " << Config::server().timeout_seconds << "s (expected: 300)" << std::endl;
    std::cout << std::endl;
    std::cout << "Redis Host: " << Config::redis().host << " (expected: 127.0.0.1)" << std::endl;
    std::cout << "Redis Port: " << Config::redis().port << " (expected: 6379)" << std::endl;
    std::cout << "Redis DB: " << Config::redis().db << " (expected: 0)" << std::endl;
    std::cout << std::endl;
    std::cout << "Log Level: " << Config::log().level << " (expected: info)" << std::endl;
    std::cout << "Log File: " << Config::log().file << " (expected: gateway.log)" << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    
    return 0;
}
