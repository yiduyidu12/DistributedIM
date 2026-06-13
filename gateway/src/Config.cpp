// Config - 配置管理类
// 负责加载和管理服务器配置，支持 INI 和 JSON 两种配置文件格式
// 提供配置验证和默认值机制，确保配置的有效性

#include "Config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <set>

// 静态成员变量初始化
Config::ServerConfig Config::server_config_;
Config::RedisConfig Config::redis_config_;
Config::LogConfig Config::log_config_;
Config::DebugConfig Config::debug_config_;

// 匿名命名空间 - 内部辅助函数
namespace {
    // 去除字符串两端的空白字符
    // 参数 s: 要处理的字符串
    // 返回值: 处理后的字符串
    std::string trim(const std::string& s) {
        auto start = s.begin();
        while (start != s.end() && std::isspace(*start)) start++;
        auto end = s.end();
        do { end--; } while (std::distance(start, end) > 0 && std::isspace(*end));
        return std::string(start, end + 1);
    }

    // 将字符串解析为布尔值
    // 参数 value: 要解析的字符串
    // 返回值: 解析后的布尔值
    bool parseBool(const std::string& value) {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower == "true" || lower == "1" || lower == "yes";
    }

    // 安全解析整数值，带有范围检查和默认值
    // 参数 value: 要解析的字符串
    // 参数 result: 解析结果输出参数
    // 参数 min: 最小值
    // 参数 max: 最大值
    // 参数 defaultValue: 默认值
    // 参数 file: 配置文件名（用于日志）
    // 参数 line: 行号（用于日志）
    // 参数 key: 配置键名（用于日志）
    // 返回值: 成功返回true，失败返回false
    bool parseIntSafe(const std::string& value, int& result, int min, int max, int defaultValue, 
                     const std::string& file, int line, const std::string& key) {
        try {
            int val = std::stoi(value);
            if (val < min || val > max) {
                std::cerr << "[WARN] [" << file << ":" << line << "] " 
                          << "Value '" << val << "' for '" << key << "' is out of range [" 
                          << min << "-" << max << "], using default: " << defaultValue << std::endl;
                result = defaultValue;
                return false;
            }
            result = val;
            return true;
        } catch (const std::invalid_argument&) {
            std::cerr << "[WARN] [" << file << ":" << line << "] " 
                      << "Invalid integer value '" << value << "' for '" << key 
                      << "', using default: " << defaultValue << std::endl;
            result = defaultValue;
            return false;
        } catch (const std::out_of_range&) {
            std::cerr << "[WARN] [" << file << ":" << line << "] " 
                      << "Integer value '" << value << "' for '" << key 
                      << "' is out of range, using default: " << defaultValue << std::endl;
            result = defaultValue;
            return false;
        }
    }

    // 验证日志级别是否有效
    // 参数 level: 日志级别字符串
    // 返回值: 有效返回true，无效返回false
    bool isValidLogLevel(const std::string& level) {
        static const std::set<std::string> valid_levels = {"trace", "debug", "info", "warn", "error", "critical"};
        return valid_levels.count(level) > 0;
    }

    // 验证主机名格式是否有效
    // 参数 host: 主机名字符串
    // 返回值: 有效返回true，无效返回false
    bool isValidHost(const std::string& host) {
        if (host.empty()) return false;
        if (host == "localhost") return true;
        // 接受 IPv4 地址和 DNS 主机名（字母、数字、点、连字符）
        if (host.front() == '.' || host.back() == '.')
            return false;
        for (char c : host) {
            if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-')
                return false;
        }
        return true;
    }
}

// 加载配置文件
// 根据文件扩展名自动选择 INI 或 JSON 格式解析
// 参数 config_file: 配置文件路径
// 返回值: 成功返回true，失败返回false
bool Config::load(const std::string& config_file) {
    size_t dot_pos = config_file.find_last_of('.');
    std::string ext;
    if (dot_pos != std::string::npos) {
        ext = config_file.substr(dot_pos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    
    bool loaded = false;
    if (ext == "ini") {
        loaded = loadIni(config_file);
    } else {
        loaded = loadJson(config_file);
    }
    
    if (loaded) {
        validateConfig(config_file);
    }
    
    return loaded;
}

// 加载 JSON 格式配置文件
// 参数 config_file: JSON配置文件路径
// 返回值: 成功返回true，失败返回false
bool Config::loadJson(const std::string& config_file) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "[WARN] Config file not found: '" << config_file 
                  << "', using default configuration" << std::endl;
        return false;
    }
    
    try {
        nlohmann::json config;
        file >> config;
        
        // 解析服务器配置
        if (config.contains("server")) {
            auto& server = config["server"];
            if (server.is_object()) {
                int val;
                if (server.contains("port") && server["port"].is_number_integer()) {
                    val = server["port"];
                    if (val < 1 || val > 65535) {
                        std::cerr << "[WARN] [JSON] Server port " << val << " is out of range [1-65535], using default: " 
                                  << server_config_.port << std::endl;
                    } else {
                        server_config_.port = val;
                    }
                }
                if (server.contains("max_connections") && server["max_connections"].is_number_integer()) {
                    val = server["max_connections"];
                    server_config_.max_connections = val > 0 ? val : server_config_.max_connections;
                }
                if (server.contains("timeout_seconds") && server["timeout_seconds"].is_number_integer()) {
                    val = server["timeout_seconds"];
                    server_config_.timeout_seconds = val > 0 ? val : server_config_.timeout_seconds;
                }
            } else {
                std::cerr << "[WARN] [JSON] 'server' section is not an object, using defaults" << std::endl;
            }
        }
        
        // 解析 Redis 配置
        if (config.contains("redis")) {
            auto& redis = config["redis"];
            if (redis.is_object()) {
                if (redis.contains("host") && redis["host"].is_string()) {
                    std::string host = redis["host"];
                    if (!isValidHost(host)) {
                        std::cerr << "[WARN] [JSON] Invalid Redis host format: '" << host 
                                  << "', using default: " << redis_config_.host << std::endl;
                    } else {
                        redis_config_.host = host;
                    }
                }
                if (redis.contains("port") && redis["port"].is_number_integer()) {
                    int val = redis["port"];
                    redis_config_.port = (val > 0 && val <= 65535) ? val : redis_config_.port;
                }
                if (redis.contains("password") && redis["password"].is_string()) {
                    redis_config_.password = redis["password"];
                }
                if (redis.contains("db") && redis["db"].is_number_integer()) {
                    int val = redis["db"];
                    redis_config_.db = (val >= 0 && val < 16) ? val : redis_config_.db;
                }
            }
        }
        
        // 解析日志配置
        if (config.contains("log")) {
            auto& log = config["log"];
            if (log.is_object()) {
                if (log.contains("level") && log["level"].is_string()) {
                    std::string level = log["level"];
                    std::transform(level.begin(), level.end(), level.begin(), ::tolower);
                    if (!isValidLogLevel(level)) {
                        std::cerr << "[WARN] [JSON] Invalid log level: '" << level
                                  << "', using default: " << log_config_.level << std::endl;
                    } else {
                        log_config_.level = level;
                    }
                }
                if (log.contains("file") && log["file"].is_string()) {
                    std::string file = log["file"];
                    if (!file.empty()) {
                        log_config_.file = file;
                    }
                }
                if (log.contains("console_output") && log["console_output"].is_boolean()) {
                    log_config_.console_output = log["console_output"];
                }
                if (log.contains("file_output") && log["file_output"].is_boolean()) {
                    log_config_.file_output = log["file_output"];
                }
                if (log.contains("max_file_size") && log["max_file_size"].is_number_integer()) {
                    int val = log["max_file_size"];
                    log_config_.max_file_size = (val > 0) ? val : log_config_.max_file_size;
                }
                if (log.contains("max_files") && log["max_files"].is_number_integer()) {
                    int val = log["max_files"];
                    log_config_.max_files = (val > 0) ? val : log_config_.max_files;
                }
                if (log.contains("console_pattern") && log["console_pattern"].is_string()) {
                    log_config_.console_pattern = log["console_pattern"];
                }
                if (log.contains("file_pattern") && log["file_pattern"].is_string()) {
                    log_config_.file_pattern = log["file_pattern"];
                }
            }
        }
        
        std::cout << "[INFO] Config loaded successfully from: " << config_file << std::endl;
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[ERROR] [JSON] Parse error at position " << e.byte << ": " << e.what() << std::endl;
        std::cerr << "[ERROR] [JSON] Failed to parse config file: '" << config_file 
                  << "', using default configuration" << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] [JSON] Unexpected error loading config: " << e.what() << std::endl;
        return false;
    }
}

// 加载 INI 格式配置文件
// 参数 config_file: INI配置文件路径
// 返回值: 成功返回true，失败返回false
bool Config::loadIni(const std::string& config_file) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "[WARN] Config file not found: '" << config_file 
                  << "', using default configuration" << std::endl;
        return false;
    }
    
    std::string current_section;
    std::string line;
    int line_num = 0;
    bool has_errors = false;
    
    while (std::getline(file, line)) {
        line_num++;
        
        // 处理注释
        size_t comment_pos = line.find(';');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        
        line = trim(line);
        if (line.empty()) continue;
        
        // 处理 section
        if (line.front() == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            std::transform(current_section.begin(), current_section.end(), current_section.begin(), ::tolower);
            continue;
        }
        
        // 解析键值对
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                      << "Invalid line format (missing '='): '" << line << "'" << std::endl;
            has_errors = true;
            continue;
        }
        
        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));
        
        if (key.empty()) {
            std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                      << "Empty key name in line: '" << line << "'" << std::endl;
            has_errors = true;
            continue;
        }
        
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        
        try {
            // 根据 section 解析配置
            if (current_section == "server") {
                if (key == "port") {
                    parseIntSafe(value, server_config_.port, 1, 65535, server_config_.port, 
                                config_file, line_num, "server.port");
                } else if (key == "max_connections") {
                    parseIntSafe(value, server_config_.max_connections, 1, 65535, 
                                server_config_.max_connections, config_file, line_num, "server.max_connections");
                } else if (key == "timeout_seconds") {
                    parseIntSafe(value, server_config_.timeout_seconds, 1, 86400, 
                                server_config_.timeout_seconds, config_file, line_num, "server.timeout_seconds");
                } else {
                    std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                              << "Unknown key '" << key << "' in [server] section" << std::endl;
                    has_errors = true;
                }
            } else if (current_section == "redis") {
                if (key == "host") {
                    if (!isValidHost(value)) {
                        std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                                  << "Invalid host format: '" << value 
                                  << "', using default: " << redis_config_.host << std::endl;
                    } else {
                        redis_config_.host = value;
                    }
                } else if (key == "port") {
                    parseIntSafe(value, redis_config_.port, 1, 65535, redis_config_.port, 
                                config_file, line_num, "redis.port");
                } else if (key == "password") {
                    redis_config_.password = value;
                } else if (key == "db") {
                    parseIntSafe(value, redis_config_.db, 0, 15, redis_config_.db, 
                                config_file, line_num, "redis.db");
                } else {
                    std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                              << "Unknown key '" << key << "' in [redis] section" << std::endl;
                    has_errors = true;
                }
            } else if (current_section == "log") {
                if (key == "level") {
                    std::string level = value;
                    std::transform(level.begin(), level.end(), level.begin(), ::tolower);
                    if (!isValidLogLevel(level)) {
                        std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                                  << "Invalid log level: '" << value 
                                  << "', valid values: trace, debug, info, warn, error, critical. "
                                  << "Using default: " << log_config_.level << std::endl;
                    } else {
                        log_config_.level = level;
                    }
                } else if (key == "file") {
                    if (value.empty()) {
                        std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                                  << "Log file path is empty, using default: " << log_config_.file << std::endl;
                    } else {
                        log_config_.file = value;
                    }
                } else if (key == "console_output") {
                    log_config_.console_output = parseBool(value);
                } else if (key == "file_output") {
                    log_config_.file_output = parseBool(value);
                } else if (key == "max_file_size") {
                    parseIntSafe(value, log_config_.max_file_size, 1, 1000, log_config_.max_file_size, 
                                config_file, line_num, "log.max_file_size");
                } else if (key == "max_files") {
                    parseIntSafe(value, log_config_.max_files, 1, 100, log_config_.max_files, 
                                config_file, line_num, "log.max_files");
                } else if (key == "console_pattern" || key == "file_pattern") {
                    if (key == "console_pattern") log_config_.console_pattern = value;
                    else log_config_.file_pattern = value;
                } else {
                    std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                              << "Unknown key '" << key << "' in [log] section" << std::endl;
                    has_errors = true;
                }
            } else if (current_section == "debug") {
                if (key == "enabled") {
                    debug_config_.enabled = parseBool(value);
                } else if (key == "show_source") {
                    debug_config_.show_source = parseBool(value);
                } else {
                    std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                              << "Unknown key '" << key << "' in [debug] section" << std::endl;
                    has_errors = true;
                }
            } else {
                std::cerr << "[WARN] [" << config_file << ":" << line_num << "] " 
                          << "No section defined for key '" << key << "'" << std::endl;
                has_errors = true;
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] [" << config_file << ":" << line_num << "] " 
                      << "Error processing key '" << key << "': " << e.what() << std::endl;
            has_errors = true;
        }
    }
    
    if (has_errors) {
        std::cout << "[INFO] INI config loaded from: " << config_file 
                  << " with warnings (see above), using defaults for invalid values" << std::endl;
    } else {
        std::cout << "[INFO] INI config loaded successfully from: " << config_file << std::endl;
    }
    
    return true;
}

// 验证配置有效性
// 检查所有配置项是否在有效范围内，无效值使用默认值替代
// 参数 config_file: 配置文件路径（用于日志）
void Config::validateConfig(const std::string& config_file) {
    bool has_warnings = false;
    
    // 检查服务器配置
    if (server_config_.port < 1 || server_config_.port > 65535) {
        std::cerr << "[WARN] [Validation] Server port " << server_config_.port 
                  << " is invalid, using default: 8888" << std::endl;
        server_config_.port = 8888;
        has_warnings = true;
    }
    
    if (server_config_.max_connections < 1) {
        std::cerr << "[WARN] [Validation] Max connections " << server_config_.max_connections 
                  << " is invalid, using default: 1024" << std::endl;
        server_config_.max_connections = 1024;
        has_warnings = true;
    }
    
    // 检查 Redis 配置
    if (redis_config_.host.empty() || !isValidHost(redis_config_.host)) {
        std::cerr << "[WARN] [Validation] Redis host '" << redis_config_.host 
                  << "' is invalid, using default: 127.0.0.1" << std::endl;
        redis_config_.host = "127.0.0.1";
        has_warnings = true;
    }
    
    if (redis_config_.port < 1 || redis_config_.port > 65535) {
        std::cerr << "[WARN] [Validation] Redis port " << redis_config_.port 
                  << " is invalid, using default: 6379" << std::endl;
        redis_config_.port = 6379;
        has_warnings = true;
    }
    
    // 检查日志配置
    if (!isValidLogLevel(log_config_.level)) {
        std::cerr << "[WARN] [Validation] Log level '" << log_config_.level 
                  << "' is invalid, using default: info" << std::endl;
        log_config_.level = "info";
        has_warnings = true;
    }
    
    if (log_config_.file.empty()) {
        std::cerr << "[WARN] [Validation] Log file path is empty, using default: gateway.log" << std::endl;
        log_config_.file = "gateway.log";
        has_warnings = true;
    }
    
    if (!log_config_.console_output && !log_config_.file_output) {
        std::cerr << "[WARN] [Validation] Both console_output and file_output are disabled, "
                  << "enabling console output" << std::endl;
        log_config_.console_output = true;
        has_warnings = true;
    }
    
    if (has_warnings) {
        std::cout << "[INFO] Configuration validation completed with warnings" << std::endl;
    }
}

// 获取服务器配置
// 返回值: 服务器配置常量引用
const Config::ServerConfig& Config::server() {
    return server_config_;
}

// 获取 Redis 配置
// 返回值: Redis配置常量引用
const Config::RedisConfig& Config::redis() {
    return redis_config_;
}

// 获取日志配置
// 返回值: 日志配置常量引用
const Config::LogConfig& Config::log() {
    return log_config_;
}

// 获取调试配置
// 返回值: 调试配置常量引用
const Config::DebugConfig& Config::debug() {
    return debug_config_;
}