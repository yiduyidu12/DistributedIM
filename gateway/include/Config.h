#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

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

    // 新增：E2EE 加密配置
    struct E2EEConfig {
        bool enabled = false;            // 是否启用端到端加密
        std::string key_dir = "./keys";  // 密钥存储目录
    };

    // 新增：AI 服务配置
    struct AIConfig {
        bool enabled = false;
        std::string api_url = "http://ai-service:8000";
        std::string audit_api_url = "http://http-api:8080"; // 审计日志 API 地址
        int timeout_ms = 30000;          // AI 调用超时（毫秒）
        int max_context_messages = 20;   // 对话上下文最大消息数
    };

    // 新增：区域配置（地理感知路由）
    struct RegionConfig {
        std::string region = "default";  // 网关区域标识
        bool prefer_local = true;        // 是否优先本地路由
    };

    // 新增：限流配置
    struct RateLimitConfig {
        bool enabled = true;
        double messages_per_second = 10.0;  // 每连接每秒最大消息数
        int bucket_capacity = 20;           // 令牌桶容量
    };

    // 新增：WebSocket 配置
    struct WebSocketConfig {
        bool enabled = true;
        uint64_t max_frame_size = 65536;    // 最大帧载荷
        int max_message_size = 1048576;     // 最大重组消息（1MB）
    };

    static bool load(const std::string& config_file);
    static bool loadJson(const std::string& config_file);
    static bool loadIni(const std::string& config_file);
    static void validateConfig(const std::string& config_file);

    // 原有配置访问器
    static const ServerConfig& server();
    static const RedisConfig& redis();
    static const LogConfig& log();
    static const DebugConfig& debug();

    // 新增配置访问器
    static const E2EEConfig& e2ee();
    static const AIConfig& ai();
    static const RegionConfig& region();
    static const RateLimitConfig& rateLimit();
    static const WebSocketConfig& websocket();

    static void parseNewJsonConfig(const nlohmann::json& j);

private:
    static ServerConfig server_config_;
    static RedisConfig redis_config_;
    static LogConfig log_config_;
    static DebugConfig debug_config_;
    static E2EEConfig e2ee_config_;
    static AIConfig ai_config_;
    static RegionConfig region_config_;
    static RateLimitConfig ratelimit_config_;
    static WebSocketConfig ws_config_;
};
