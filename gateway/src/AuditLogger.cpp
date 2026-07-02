// AuditLogger - 审计日志记录器实现
// 异步批量发送审计日志到 Go API 服务，避免阻塞主线程
// 使用独立的后台线程处理 HTTP POST 发送

#include "AuditLogger.h"
#include "Logger.h"
#include <curl/curl.h>
#include <chrono>

// 将审计事件类型转为字符串
// 参数 type: 审计事件类型枚举
// 返回值: 事件类型字符串
static std::string eventTypeToString(AuditEventType type) {
    switch (type) {
        case AuditEventType::LOGIN_ATTEMPT:     return "LOGIN_ATTEMPT";
        case AuditEventType::LOGOUT:            return "LOGOUT";
        case AuditEventType::MESSAGE_SENT:      return "MESSAGE_SENT";
        case AuditEventType::GROUP_CREATED:     return "GROUP_CREATED";
        case AuditEventType::GROUP_JOINED:      return "GROUP_JOINED";
        case AuditEventType::GROUP_LEFT:        return "GROUP_LEFT";
        case AuditEventType::RATE_LIMIT_HIT:    return "RATE_LIMIT_HIT";
        case AuditEventType::CONNECTION_DROP:   return "CONNECTION_DROP";
        case AuditEventType::E2EE_KEY_EXCHANGE: return "E2EE_KEY_EXCHANGE";
        default: return "UNKNOWN";
    }
}

// 审计条目转 JSON 字符串
// 返回值: JSON 格式的审计日志字符串
std::string AuditEntry::toJson() const {
    nlohmann::json j;
    j["event_type"] = eventTypeToString(event_type);
    j["username"]   = username;
    j["target"]     = target;
    j["detail"]     = detail;
    j["ip_address"] = ip_address;
    j["timestamp"]  = timestamp;
    return j.dump();
}

// 单例获取
AuditLogger& AuditLogger::instance() {
    static AuditLogger logger;
    return logger;
}

// 构造函数（私有）
AuditLogger::AuditLogger() : running_(false), batch_size_(50) {}

// 析构函数
AuditLogger::~AuditLogger() {
    shutdown();
}

// 初始化审计日志器
// 参数 api_url: Go API 服务地址
// 参数 batch_size: 批量发送的阈值
void AuditLogger::init(const std::string& api_url, size_t batch_size) {
    api_url_ = api_url;
    batch_size_ = batch_size;
    running_ = true;
    worker_ = std::thread(&AuditLogger::workerLoop, this);
    Logger::info("[Audit] 审计日志器已启动: url={}, batch_size={}", api_url, batch_size);
}

// 关闭审计日志器
// 等待后台线程完成剩余日志发送
void AuditLogger::shutdown() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

// 记录审计事件（异步入队）
// 参数 entry: 审计条目
void AuditLogger::log(const AuditEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(entry);
}

// 便捷方法：记录登录尝试
// 参数 username: 用户名
// 参数 success: 是否成功
// 参数 ip: 客户端IP
// 参数 reason: 失败原因（可选）
void AuditLogger::logLoginAttempt(const std::string& username, bool success,
                                   const std::string& ip, const std::string& reason) {
    AuditEntry entry;
    entry.event_type = AuditEventType::LOGIN_ATTEMPT;
    entry.username = username;
    entry.ip_address = ip;
    entry.timestamp = time(nullptr);
    if (!success) entry.detail = reason;
    log(entry);
}

// 便捷方法：记录消息发送
// 参数 from: 发送者
// 参数 to: 接收者
// 参数 msg_type: 消息类型（chat/send/group_send）
// 参数 length: 消息长度
void AuditLogger::logMessageSent(const std::string& from, const std::string& to,
                                  const std::string& msg_type, size_t length) {
    AuditEntry entry;
    entry.event_type = AuditEventType::MESSAGE_SENT;
    entry.username = from;
    entry.target = to;
    entry.detail = "{\"type\":\"" + msg_type + "\",\"length\":" + std::to_string(length) + "}";
    entry.timestamp = time(nullptr);
    log(entry);
}

// 便捷方法：记录限流触发
// 参数 username: 用户名
// 参数 retry_after: 重试等待秒数
void AuditLogger::logRateLimitHit(const std::string& username, double retry_after) {
    AuditEntry entry;
    entry.event_type = AuditEventType::RATE_LIMIT_HIT;
    entry.username = username;
    entry.detail = "{\"retry_after\":" + std::to_string(retry_after) + "}";
    entry.timestamp = time(nullptr);
    log(entry);
}

// 便捷方法：记录群组操作
// 参数 op: 操作类型
// 参数 group_id: 群组ID
// 参数 username: 用户名
void AuditLogger::logGroupOperation(AuditEventType op, const std::string& group_id,
                                     const std::string& username) {
    AuditEntry entry;
    entry.event_type = op;
    entry.username = username;
    entry.target = group_id;
    entry.timestamp = time(nullptr);
    log(entry);
}

// 后台工作线程
// 定期检查队列大小，达到阈值或超过 5 秒时批量发送
void AuditLogger::workerLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        flushBatch();
    }
    // 退出前最后一次刷新
    flushBatch();
}

// 批量发送审计日志到 API
// 使用 libcurl 发送 HTTP POST 请求
void AuditLogger::flushBatch() {
    std::vector<AuditEntry> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return;

        while (!queue_.empty() && batch.size() < batch_size_) {
            batch.push_back(queue_.front());
            queue_.pop();
        }
    }

    if (batch.empty()) return;

    // 构建 JSON 数组
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& entry : batch) {
        payload.push_back(nlohmann::json::parse(entry.toJson()));
    }
    std::string body = payload.dump();

    // 使用 libcurl 发送
    CURL* curl = curl_easy_init();
    if (!curl) {
        Logger::error("[Audit] curl_easy_init 失败，{} 条日志丢失", batch.size());
        return;
    }

    std::string api_url = api_url_ + "/api/audit";
    curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        Logger::warn("[Audit] 批量发送失败 ({} 条日志): {}", batch.size(),
                     curl_easy_strerror(res));
    } else {
        Logger::trace("[Audit] 批量发送成功: {} 条日志", batch.size());
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
