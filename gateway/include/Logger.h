#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <string>
#include <mutex>

class Logger {
public:
    static void init(const std::string& log_file = "gateway.log", 
                     const std::string& level = "info");
    
    static std::shared_ptr<spdlog::logger>& get();
    
    // ============ 基础日志方法 ============
    template<typename... Args>
    static void trace(const char* fmt, Args&&... args) {
        get()->trace(fmt, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void debug(const char* fmt, Args&&... args) {
        get()->debug(fmt, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void info(const char* fmt, Args&&... args) {
        get()->info(fmt, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void warn(const char* fmt, Args&&... args) {
        get()->warn(fmt, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void error(const char* fmt, Args&&... args) {
        get()->error(fmt, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void critical(const char* fmt, Args&&... args) {
        get()->critical(fmt, std::forward<Args>(args)...);
    }

    // ============ 连接管理日志 ============
    
    // 客户端连接
    static void clientConnected(int fd, const std::string& ip = "", int port = 0, int total_connections = 0);
    
    // 客户端断开连接
    static void clientDisconnected(int fd, const std::string& username = "", int total_connections = 0);
    
    // 用户登录
    static void userLogin(int fd, const std::string& username, int gateway_id, int online_count);
    
    // 用户登录失败
    static void userLoginFailed(int fd, const std::string& username, const std::string& reason);
    
    // 用户登出
    static void userLogout(int fd, const std::string& username);
    
    // 用户登出失败
    static void userLogoutFailed(int fd, const std::string& username);
    
    // 重复登录检测
    static void duplicateLogin(const std::string& username, int old_fd, int new_fd);

    // ============ 心跳检测日志 ============
    
    // 收到心跳请求
    static void heartbeatReceived(int fd, const std::string& username);
    
    // 发送心跳响应
    static void heartbeatSent(int fd);

    // ============ 消息处理日志 ============
    
    // 收到消息
    static void messageReceived(int fd, const std::string& username, size_t length, const std::string& type = "");
    
    // 发送消息
    static void messageSent(int fd, const std::string& username, size_t length);
    
    // 消息处理错误
    static void messageError(int fd, const std::string& username, const std::string& error);
    
    // 未授权访问
    static void unauthorizedAccess(int fd, const std::string& message_type);
    
    // 未知命令
    static void unknownCommand(int fd, const std::string& command);

    // ============ Redis 操作日志 ============
    
    // Redis 连接成功
    static void redisConnected(const std::string& host, int port);
    
    // Redis 连接失败
    static void redisConnectionFailed(const std::string& host, int port, const std::string& error);
    
    // Redis 操作成功
    static void redisOperation(const std::string& operation, const std::string& key = "");
    
    // Redis 操作失败
    static void redisOperationFailed(const std::string& operation, const std::string& error);

    // ============ 服务器日志 ============
    
    // 服务器启动
    static void serverStarted(int port, int max_connections, int timeout);
    
    // 服务器关闭
    static void serverStopped();
    
    // 端口占用
    static void portInUse(int port);
    
    // 连接超时
    static void connectionTimeout(int fd, const std::string& username);

private:
    static std::shared_ptr<spdlog::logger> logger_;
    static std::mutex mutex_;
    
    static std::string formatUsername(const std::string& username) {
        return username.empty() ? "(unknown)" : username;
    }
};
