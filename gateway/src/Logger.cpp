// Logger - 日志管理类
// 基于 spdlog 封装，提供线程安全的日志记录功能
// 支持多种日志级别（trace、debug、info、warn、error、critical）
// 支持控制台和文件双输出，可通过配置控制

#include "Logger.h"
#include "Config.h"
#include <spdlog/spdlog.h>
#include <spdlog/pattern_formatter.h>
#include <mutex>

// 静态成员变量初始化
std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;
std::mutex Logger::mutex_;

// 初始化日志系统
// 参数 log_file: 日志文件路径
// 参数 level: 日志级别
void Logger::init(const std::string& log_file, const std::string& level) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 如果已初始化，先清理
    if (logger_) {
        spdlog::drop("gateway");
        logger_ = nullptr;
    }
    
    std::vector<spdlog::sink_ptr> sinks;
    auto& log_config = Config::log();
    
    // 根据配置决定是否输出到控制台
    if (log_config.console_output) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(log_config.console_pattern);
        sinks.push_back(console_sink);
    }
    
    // 根据配置决定是否输出到文件
    if (log_config.file_output) {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true);
        file_sink->set_pattern(log_config.file_pattern);
        sinks.push_back(file_sink);
    }
    
    // 如果没有任何输出目标，至少输出到控制台
    if (sinks.empty()) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%7l%$] %v");
        sinks.push_back(console_sink);
    }
    
    // 创建日志记录器
    logger_ = std::make_shared<spdlog::logger>("gateway", sinks.begin(), sinks.end());
    
    // 设置日志级别
    spdlog::level::level_enum log_level = spdlog::level::info;
    if (level == "trace") {
        log_level = spdlog::level::trace;
    } else if (level == "debug") {
        log_level = spdlog::level::debug;
    } else if (level == "warn") {
        log_level = spdlog::level::warn;
    } else if (level == "error") {
        log_level = spdlog::level::err;
    } else if (level == "critical") {
        log_level = spdlog::level::critical;
    }
    
    logger_->set_level(log_level);
    logger_->flush_on(spdlog::level::err);
    
    spdlog::register_logger(logger_);
    logger_->info("Logger initialized with level: {}", level);
    logger_->info("Console output: {}, File output: {}", 
                  log_config.console_output ? "enabled" : "disabled",
                  log_config.file_output ? "enabled" : "disabled");
}

// 获取日志记录器实例（懒加载）
// 返回值: 日志记录器共享指针引用
std::shared_ptr<spdlog::logger>& Logger::get() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!logger_) {
        init();
    }
    return logger_;
}

// ============ 连接管理日志实现 ============

// 客户端连接成功日志
// 参数 fd: 客户端文件描述符
// 参数 ip: 客户端IP地址
// 参数 port: 客户端端口
// 参数 total_connections: 当前总连接数
void Logger::clientConnected(int fd, const std::string& ip, int port, int total_connections) {
    if (!ip.empty()) {
        get()->info("[Client] New client connected: fd={}, ip={}:{}, total={}", 
                    fd, ip, port, total_connections);
    } else {
        get()->info("[Client] New client connected: fd={}, total={}", fd, total_connections);
    }
}

// 客户端断开连接日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名（可能为空）
// 参数 total_connections: 当前总连接数
void Logger::clientDisconnected(int fd, const std::string& username, int total_connections) {
    get()->info("[Client] Client disconnected: fd={}, user={}, total={}", 
                fd, formatUsername(username), total_connections);
}

// 用户登录成功日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名
// 参数 gateway_id: 网关ID
// 参数 online_count: 当前在线用户数
void Logger::userLogin(int fd, const std::string& username, int gateway_id, int online_count) {
    get()->info("[Login] SUCCESS - user='{}', fd={}, gateway={}, online_users={}", 
                username, fd, gateway_id, online_count);
}

// 用户登录失败日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名
// 参数 reason: 失败原因
void Logger::userLoginFailed(int fd, const std::string& username, const std::string& reason) {
    get()->warn("[Login] FAILED for user='{}' (fd={}) - {}", username, fd, reason);
}

// 用户登出日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名
void Logger::userLogout(int fd, const std::string& username) {
    get()->info("[Logout] User {} logged out (fd={})", username, fd);
}

// 用户登出失败日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名
void Logger::userLogoutFailed(int fd, const std::string& username) {
    get()->warn("[Logout] Failed to logout {} from Redis (fd={})", username, fd);
}

// 重复登录日志
// 参数 username: 用户名
// 参数 old_fd: 旧连接文件描述符
// 参数 new_fd: 新连接文件描述符
void Logger::duplicateLogin(const std::string& username, int old_fd, int new_fd) {
    get()->warn("[Login] Duplicate login detected: user='{}' already logged in on fd={}, replacing with fd={}", 
                username, old_fd, new_fd);
}

// ============ 心跳检测日志实现 ============

// 收到心跳包日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名（可能为空）
void Logger::heartbeatReceived(int fd, const std::string& username) {
    get()->trace("[Heartbeat] Ping received from fd={}, user='{}'", fd, formatUsername(username));
}

// 发送心跳响应日志
// 参数 fd: 客户端文件描述符
void Logger::heartbeatSent(int fd) {
    get()->trace("[Heartbeat] Pong response sent to fd={}", fd);
}

// ============ 消息处理日志实现 ============

// 收到消息日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名（可能为空）
// 参数 length: 消息长度
// 参数 type: 消息类型（可能为空）
void Logger::messageReceived(int fd, const std::string& username, size_t length, const std::string& type) {
    if (!type.empty()) {
        get()->trace("[Message] Received from fd={}, user='{}', type='{}', length={}", 
                     fd, formatUsername(username), type, length);
    } else {
        get()->trace("[Message] Received from fd={}, user='{}', length={}", 
                     fd, formatUsername(username), length);
    }
}

// 发送消息日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名（可能为空）
// 参数 length: 消息长度
void Logger::messageSent(int fd, const std::string& username, size_t length) {
    get()->trace("[Message] Sent to fd={}, user='{}', length={}", 
                 fd, formatUsername(username), length);
}

// 消息处理错误日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名（可能为空）
// 参数 error: 错误信息
void Logger::messageError(int fd, const std::string& username, const std::string& error) {
    get()->error("[Message] Error processing message from fd={}, user='{}': {}", 
                 fd, formatUsername(username), error);
}

// 未授权访问日志
// 参数 fd: 客户端文件描述符
// 参数 message_type: 消息类型
void Logger::unauthorizedAccess(int fd, const std::string& message_type) {
    get()->warn("[Message] Unauthorized access attempt from fd={}, type='{}' - not logged in", 
                fd, message_type);
}

// 未知命令日志
// 参数 fd: 客户端文件描述符
// 参数 command: 命令内容
void Logger::unknownCommand(int fd, const std::string& command) {
    get()->warn("[Message] Unknown command from fd={}: '{}'", fd, command);
}

// ============ Redis 操作日志实现 ============

// Redis 连接成功日志
// 参数 host: Redis主机地址
// 参数 port: Redis端口
void Logger::redisConnected(const std::string& host, int port) {
    get()->info("[Redis] Connected successfully to {}:{}", host, port);
}

// Redis 连接失败日志
// 参数 host: Redis主机地址
// 参数 port: Redis端口
// 参数 error: 错误信息
void Logger::redisConnectionFailed(const std::string& host, int port, const std::string& error) {
    get()->error("[Redis] Failed to connect to {}:{} - {}", host, port, error);
}

// Redis 操作成功日志
// 参数 operation: 操作名称
// 参数 key: 操作的键（可能为空）
void Logger::redisOperation(const std::string& operation, const std::string& key) {
    if (!key.empty()) {
        get()->debug("[Redis] {} operation succeeded for key='{}'", operation, key);
    } else {
        get()->debug("[Redis] {} operation succeeded", operation);
    }
}

// Redis 操作失败日志
// 参数 operation: 操作名称
// 参数 error: 错误信息
void Logger::redisOperationFailed(const std::string& operation, const std::string& error) {
    get()->error("[Redis] {} operation failed: {}", operation, error);
}

// ============ 服务器日志实现 ============

// 服务器启动日志
// 参数 port: 监听端口
// 参数 max_connections: 最大连接数
// 参数 timeout: 超时时间（秒）
void Logger::serverStarted(int port, int max_connections, int timeout) {
    get()->info("[Server] Gateway Server started on port {} (fd={})", port, 0);
    get()->info("[Server] Max connections: {}, Timeout: {}s", max_connections, timeout);
}

// 服务器停止日志
void Logger::serverStopped() {
    get()->info("[Server] Gateway Server stopped");
}

// 端口被占用日志
// 参数 port: 端口号
void Logger::portInUse(int port) {
    get()->error("[Server] Port {} is already in use. Try another port or wait for it to be released.", port);
}

// 连接超时日志
// 参数 fd: 客户端文件描述符
// 参数 username: 用户名（可能为空）
void Logger::connectionTimeout(int fd, const std::string& username) {
    get()->warn("[TIMEOUT] fd={} user={}", fd, formatUsername(username));
}