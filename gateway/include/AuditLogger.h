// AuditLogger - 审计日志记录器
// 记录所有安全相关操作：登录尝试、消息发送、群组操作、限流触发
// 审计日志通过 HTTP POST 发送到 Go API 服务 `/api/audit` 端点
// 保持 C++ 网关无状态，不直接连接 PostgreSQL

#ifndef AUDIT_LOGGER_H
#define AUDIT_LOGGER_H

#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <nlohmann/json.hpp>

// 审计事件类型
enum class AuditEventType {
    LOGIN_ATTEMPT,      // 登录尝试（成功/失败）
    LOGOUT,             // 用户登出
    MESSAGE_SENT,       // 消息发送
    GROUP_CREATED,      // 群组创建
    GROUP_JOINED,       // 加入群组
    GROUP_LEFT,         // 离开群组
    RATE_LIMIT_HIT,     // 触发限流
    CONNECTION_DROP,    // 连接断开
    E2EE_KEY_EXCHANGE,  // E2E 密钥交换
};

// 审计日志条目
struct AuditEntry {
    AuditEventType event_type;
    std::string    username;
    std::string    target;        // 目标用户/群组
    std::string    detail;        // 详细信息（JSON）
    std::string    ip_address;    // 客户端IP
    time_t         timestamp;     // 事件时间戳

    // 转换为 JSON 字符串
    std::string toJson() const;
};

// 审计日志记录器
// 异步批量发送审计日志到 Go API 服务，避免阻塞主线程
class AuditLogger {
public:
    // 获取单例实例
    static AuditLogger& instance();

    // 初始化审计日志器
    // 参数 api_url: Go API 服务地址（如 http://http-api:8080）
    // 参数 batch_size: 批量发送的阈值，默认50条
    void init(const std::string& api_url, size_t batch_size = 50);

    // 关闭审计日志器
    // 等待所有待发送日志完成
    void shutdown();

    // 记录审计事件
    // 参数 entry: 审计条目（异步入队，不阻塞调用方）
    void log(const AuditEntry& entry);

    // 便捷方法：记录登录尝试
    void logLoginAttempt(const std::string& username, bool success,
                         const std::string& ip, const std::string& reason = "");

    // 便捷方法：记录消息发送
    void logMessageSent(const std::string& from, const std::string& to,
                        const std::string& msg_type, size_t length);

    // 便捷方法：记录限流触发
    void logRateLimitHit(const std::string& username, double retry_after);

    // 便捷方法：记录群组操作
    void logGroupOperation(AuditEventType op, const std::string& group_id,
                           const std::string& username);

private:
    // 构造函数（私有，单例模式）
    AuditLogger();

    // 析构函数
    ~AuditLogger();

    // 禁止拷贝
    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    // 后台工作线程
    void workerLoop();

    // 批量发送审计日志到 API
    void flushBatch();

    std::queue<AuditEntry> queue_;   // 审计日志队列
    std::mutex              mutex_;  // 队列互斥锁
    std::thread             worker_; // 后台发送线程
    std::atomic<bool>       running_; // 运行标志

    std::string api_url_;    // Go API 地址
    size_t      batch_size_; // 批量发送阈值
};

#endif // AUDIT_LOGGER_H
