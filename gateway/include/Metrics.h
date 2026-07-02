// Metrics - Prometheus 监控指标导出器
// 提供 HTTP 端点 /metrics 供 Prometheus 抓取
// 导出的指标包括：连接数、消息吞吐量、延迟分位数、Redis 操作数、AI 调用量

#ifndef METRICS_H
#define METRICS_H

#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <ctime>

// Prometheus 指标导出器
// 线程安全，支持 Counter、Gauge、Histogram 三种指标类型
class Metrics {
public:
    // 获取单例实例
    static Metrics& instance();

    // ============ Counter 指标（单调递增） ============

    // 增加连接总数
    void incConnections();

    // 减少连接总数
    void decConnections();

    // 增加消息计数
    // 参数 type: 消息类型（chat/send/ping/group_send）
    void incMessages(const std::string& type);

    // 增加消息发送字节数
    // 参数 bytes: 发送字节数
    void addBytesSent(uint64_t bytes);

    // 增加消息接收字节数
    // 参数 bytes: 接收字节数
    void addBytesReceived(uint64_t bytes);

    // 增加 Redis 操作计数
    // 参数 operation: 操作名称（login/logout/push/pop）
    void incRedisOps(const std::string& operation);

    // 增加 AI 调用计数
    // 参数 provider: AI 提供商名称
    // 参数 success: 是否成功
    void incAICalls(const std::string& provider, bool success);

    // 增加限流触发计数
    void incRateLimitHits();

    // ============ Gauge 指标（瞬时值） ============

    // 设置当前在线用户数
    // 参数 count: 在线用户数
    void setOnlineUsers(int count);

    // 设置当前活跃 WebSocket 连接数
    // 参数 count: 连接数
    void setActiveWSConnections(int count);

    // 设置 ACK 待确认消息数
    // 参数 count: 待确认消息数
    void setPendingAcks(int count);

    // ============ Histogram 指标（分布统计） ============

    // 记录消息处理延迟
    // 参数 latency_ms: 延迟（毫秒）
    void recordMessageLatency(double latency_ms);

    // 记录 AI 调用延迟
    // 参数 latency_ms: 延迟（毫秒）
    // 参数 provider: AI 提供商名称
    void recordAILatency(const std::string& provider, double latency_ms);

    // ============ 导出 ============

    // 生成 Prometheus 格式的指标文本
    // 返回值: Prometheus 文本格式的指标数据
    std::string exportPrometheusText() const;

    // 重置所有指标（用于测试）
    void reset();

private:
    // 构造函数（私有，单例模式）
    Metrics();

    // 禁止拷贝
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    mutable std::mutex mutex_;

    // Counter 指标
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<uint64_t> total_messages_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> redis_ops_{0};
    std::atomic<uint64_t> ai_calls_{0};
    std::atomic<uint64_t> rate_limit_hits_{0};

    // 按类型/操作分类的计数器
    std::unordered_map<std::string, std::atomic<uint64_t>> messages_by_type_;
    std::unordered_map<std::string, std::atomic<uint64_t>> redis_ops_by_type_;
    std::unordered_map<std::string, std::atomic<uint64_t>> ai_calls_by_provider_;
    std::unordered_map<std::string, std::atomic<uint64_t>> ai_calls_failed_;

    // Gauge 指标
    std::atomic<int> online_users_{0};
    std::atomic<int> active_ws_connections_{0};
    std::atomic<int> pending_acks_{0};

    // Histogram 延迟样本（简化实现：存储最近 N 个样本）
    mutable std::vector<double> message_latencies_;
    mutable std::unordered_map<std::string, std::vector<double>> ai_latencies_;
    static constexpr size_t MAX_LATENCY_SAMPLES = 10000;
};

#endif // METRICS_H
