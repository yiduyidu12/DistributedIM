// Metrics - Prometheus 监控指标导出器实现
// 提供 Counter/Gauge/Histogram 三种指标类型
// 导出格式符合 Prometheus 文本格式规范

#include "Metrics.h"
#include <sstream>
#include <algorithm>
#include <cmath>

// 单例获取
Metrics& Metrics::instance() {
    static Metrics m;
    return m;
}

// 构造函数（私有）
Metrics::Metrics() {}

// ============ Counter 指标 ============

void Metrics::incConnections() { total_connections_++; }
void Metrics::decConnections() { total_connections_--; }

// 增加消息计数
// 参数 type: 消息类型
void Metrics::incMessages(const std::string& type) {
    total_messages_++;
    // 加锁保护：避免并发写入 messages_by_type_
    std::lock_guard<std::mutex> lock(mutex_);
    messages_by_type_[type]++;
}

// 增加发送字节数
// 参数 bytes: 字节数
void Metrics::addBytesSent(uint64_t bytes) { bytes_sent_ += bytes; }

// 增加接收字节数
// 参数 bytes: 字节数
void Metrics::addBytesReceived(uint64_t bytes) { bytes_received_ += bytes; }

// 增加 Redis 操作计数
// 参数 operation: 操作名称
void Metrics::incRedisOps(const std::string& operation) {
    redis_ops_++;
    // 加锁保护：避免并发写入 redis_ops_by_type_
    std::lock_guard<std::mutex> lock(mutex_);
    redis_ops_by_type_[operation]++;
}

// 增加 AI 调用计数
// 参数 provider: AI 提供商名称
// 参数 success: 是否成功
void Metrics::incAICalls(const std::string& provider, bool success) {
    ai_calls_++;
    // 加锁保护：避免并发写入 ai_calls_by_provider_ 和 ai_calls_failed_
    std::lock_guard<std::mutex> lock(mutex_);
    ai_calls_by_provider_[provider]++;
    if (!success) ai_calls_failed_[provider]++;
}

// 增加限流触发计数
void Metrics::incRateLimitHits() { rate_limit_hits_++; }

// ============ Gauge 指标 ============

// 设置当前在线用户数
// 参数 count: 在线用户数
void Metrics::setOnlineUsers(int count) { online_users_ = count; }

// 设置当前活跃 WebSocket 连接数
// 参数 count: 连接数
void Metrics::setActiveWSConnections(int count) { active_ws_connections_ = count; }

// 设置 ACK 待确认消息数
// 参数 count: 待确认消息数
void Metrics::setPendingAcks(int count) { pending_acks_ = count; }

// ============ Histogram 指标 ============

// 记录消息处理延迟
// 参数 latency_ms: 延迟（毫秒）
void Metrics::recordMessageLatency(double latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (message_latencies_.size() < MAX_LATENCY_SAMPLES) {
        message_latencies_.push_back(latency_ms);
    }
}

// 记录 AI 调用延迟
// 参数 latency_ms: 延迟（毫秒）
// 参数 provider: AI 提供商名称
void Metrics::recordAILatency(const std::string& provider, double latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& samples = ai_latencies_[provider];
    if (samples.size() < MAX_LATENCY_SAMPLES) {
        samples.push_back(latency_ms);
    }
}

// ============ Prometheus 导出 ============

// 计算百分位数（简化版线性插值）
// 参数 samples: 样本数据（原地排序）
// 参数 percentile: 百分位（0-100）
// 返回值: 对应百分位的值
static double percentile(std::vector<double>& samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    double idx = (p / 100.0) * (samples.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return samples[lo];
    double frac = idx - lo;
    return samples[lo] * (1.0 - frac) + samples[hi] * frac;
}

// 生成 Prometheus 文本格式的指标数据
// 返回值: Prometheus 可抓取的文本
std::string Metrics::exportPrometheusText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;

    // HELP/TYPE 注释 + 指标值

    // --- Counter 指标 ---
    out << "# HELP im_connections_total Total connections count\n";
    out << "# TYPE im_connections_total counter\n";
    out << "im_connections_total " << total_connections_ << "\n";

    out << "# HELP im_messages_total Total messages processed\n";
    out << "# TYPE im_messages_total counter\n";
    out << "im_messages_total " << total_messages_ << "\n";

    for (const auto& [type, count] : messages_by_type_) {
        out << "im_messages_total{type=\"" << type << "\"} " << count << "\n";
    }

    out << "# HELP im_bytes_sent_total Total bytes sent\n";
    out << "# TYPE im_bytes_sent_total counter\n";
    out << "im_bytes_sent_total " << bytes_sent_ << "\n";

    out << "# HELP im_bytes_received_total Total bytes received\n";
    out << "# TYPE im_bytes_received_total counter\n";
    out << "im_bytes_received_total " << bytes_received_ << "\n";

    out << "# HELP im_redis_ops_total Total Redis operations\n";
    out << "# TYPE im_redis_ops_total counter\n";
    out << "im_redis_ops_total " << redis_ops_ << "\n";

    out << "# HELP im_ai_calls_total Total AI calls\n";
    out << "# TYPE im_ai_calls_total counter\n";
    out << "im_ai_calls_total " << ai_calls_ << "\n";
    for (const auto& [provider, count] : ai_calls_by_provider_) {
        out << "im_ai_calls_total{provider=\"" << provider << "\"} " << count << "\n";
    }
    for (const auto& [provider, count] : ai_calls_failed_) {
        out << "im_ai_calls_failed_total{provider=\"" << provider << "\"} " << count << "\n";
    }

    out << "# HELP im_rate_limit_hits_total Total rate limit hits\n";
    out << "# TYPE im_rate_limit_hits_total counter\n";
    out << "im_rate_limit_hits_total " << rate_limit_hits_ << "\n";

    // --- Gauge 指标 ---
    out << "# HELP im_online_users Current online user count\n";
    out << "# TYPE im_online_users gauge\n";
    out << "im_online_users " << online_users_ << "\n";

    out << "# HELP im_active_ws_connections Active WebSocket connections\n";
    out << "# TYPE im_active_ws_connections gauge\n";
    out << "im_active_ws_connections " << active_ws_connections_ << "\n";

    out << "# HELP im_pending_acks Pending ACK messages\n";
    out << "# TYPE im_pending_acks gauge\n";
    out << "im_pending_acks " << pending_acks_ << "\n";

    // --- Histogram 指标（P50/P95/P99） ---
    auto latencies_copy = message_latencies_;
    if (!latencies_copy.empty()) {
        out << "# HELP im_message_latency_ms Message processing latency\n";
        out << "# TYPE im_message_latency_ms summary\n";
        out << "im_message_latency_ms{quantile=\"0.5\"} " << percentile(latencies_copy, 50) << "\n";
        out << "im_message_latency_ms{quantile=\"0.95\"} " << percentile(latencies_copy, 95) << "\n";
        out << "im_message_latency_ms{quantile=\"0.99\"} " << percentile(latencies_copy, 99) << "\n";
        out << "im_message_latency_ms_count " << latencies_copy.size() << "\n";
    }

    for (auto& [provider, samples] : ai_latencies_) {
        if (!samples.empty()) {
            out << "# HELP im_ai_latency_ms AI call latency by provider\n";
            out << "# TYPE im_ai_latency_ms summary\n";
            out << "im_ai_latency_ms{provider=\"" << provider << "\",quantile=\"0.5\"} "
                << percentile(samples, 50) << "\n";
            out << "im_ai_latency_ms{provider=\"" << provider << "\",quantile=\"0.95\"} "
                << percentile(samples, 95) << "\n";
            out << "im_ai_latency_ms_count{provider=\"" << provider << "\"} " << samples.size() << "\n";
        }
    }

    return out.str();
}

// 重置所有指标（用于测试）
void Metrics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_connections_ = 0;
    total_messages_ = 0;
    bytes_sent_ = 0;
    bytes_received_ = 0;
    redis_ops_ = 0;
    ai_calls_ = 0;
    rate_limit_hits_ = 0;
    messages_by_type_.clear();
    redis_ops_by_type_.clear();
    ai_calls_by_provider_.clear();
    ai_calls_failed_.clear();
    online_users_ = 0;
    active_ws_connections_ = 0;
    pending_acks_ = 0;
    message_latencies_.clear();
    ai_latencies_.clear();
}
