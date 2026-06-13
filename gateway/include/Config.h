#pragma once

#include <string>

class Config {
public:
    struct ServerConfig {
        int port = 8888;
        int max_connections = 1024;
        int timeout_seconds = 300;
    };

    struct RedisConfig {
        std::string host = "127.0.0.1";
        int port = 6379;
        std::string password = "";
        int db = 0;
    };

    struct LogConfig {
        std::string level = "info";
        std::string file = "gateway.log";
        bool console_output = true;
        bool file_output = true;
        int max_file_size = 10;
        int max_files = 5;
        std::string console_pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%7l%$] [%25s:%-4#] %v";
        std::string file_pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [PID:%P] [TID:%t] [%s:%#] [%!] %v";
    };

    struct DebugConfig {
        bool enabled = false;
        bool show_source = true;
    };

    static bool load(const std::string& config_file);
    static bool loadJson(const std::string& config_file);
    static bool loadIni(const std::string& config_file);
    static void validateConfig(const std::string& config_file);
    
    static const ServerConfig& server();
    static const RedisConfig& redis();
    static const LogConfig& log();
    static const DebugConfig& debug();

private:
    static ServerConfig server_config_;
    static RedisConfig redis_config_;
    static LogConfig log_config_;
    static DebugConfig debug_config_;
};
