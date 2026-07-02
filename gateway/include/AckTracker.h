// AckTracker - ACK 消息确认追踪器
// 负责消息投递的可靠性保障：客户端收到消息后须回复 ACK
// 超时未确认的消息使用指数退避策略自动重试（最多3次）
// 重试间隔：1s → 2s → 4s → 放弃并移入死信队列

#ifndef ACK_TRACKER_H
#define ACK_TRACKER_H

#include <string>
#include <unordered_map>
#include <queue>
#include <ctime>
#include <cstdint>
#include <mutex>

// 单条待确认消息的追踪状态
struct AckEntry {
    std::string msg_id;       // 消息唯一ID（UUID，由客户端生成）
    std::string target_user;  // 目标用户名
    std::string payload;      // 原始消息载荷（用于重试）
    int         target_fd;    // 目标客户端的文件描述符
    time_t      send_time;    // 首次发送时间戳
    int         retry_count;  // 已重试次数（0=首次发送，最多3次）
    time_t      next_retry;   // 下次重试时间戳

    // 计算下次重试的等待秒数（指数退避：1s, 2s, 4s）
    int retryDelaySeconds() const {
        // 1 << retry_count = 1, 2, 4
        return 1 << retry_count;
    }
};

// ACK 确认追踪器
// 管理所有待确认消息的生命周期，提供重试调度和死信队列
class AckTracker {
public:
    // 构造函数
    AckTracker();

    // 注册一条待确认消息
    // 参数 msg_id: 消息唯一ID
    // 参数 target_user: 目标用户名
    // 参数 payload: 原始消息内容
    // 参数 target_fd: 目标客户端文件描述符
    void trackMessage(const std::string& msg_id, const std::string& target_user,
                      const std::string& payload, int target_fd);

    // 确认消息已被接收（客户端回复了 ACK）
    // 参数 msg_id: 消息唯一ID
    // 返回值: 确认成功返回true，未找到该消息返回false
    bool acknowledgeMessage(const std::string& msg_id);

    // 获取当前需要重试的消息列表
    // 遍历所有待确认消息，检查 next_retry 时间是否已到
    // 返回值: 需要立即重试的消息列表
    std::vector<AckEntry> getPendingRetries();

    // 标记某条消息重试已完成
    // 更新 retry_count 和 next_retry 时间
    // 参数 msg_id: 消息唯一ID
    void markRetryDone(const std::string& msg_id);

    // 获取超时放弃的消息列表（重试3次仍失败）
    // 这些消息将被移入死信队列
    // 返回值: 已放弃的消息列表
    std::vector<AckEntry> drainDeadLetters();

    // 获取待确认消息总数
    // 返回值: 当前追踪中的消息数量
    size_t pendingCount() const;

    // 清理过期条目（超过 5 分钟未确认且无更多重试机会的消息）
    void purgeExpired();

private:
    // 消息ID → 确认条目的映射表
    std::unordered_map<std::string, AckEntry> pending_acks_;

    // 互斥锁：保护 pending_acks_ 的并发访问
    // 避免主事件循环与其他线程同时修改映射表导致数据竞争
    mutable std::mutex mutex_;

    // 最大重试次数
    static constexpr int MAX_RETRIES = 3;

    // ACK 超时时间（秒），超过此时间且无更多重试机会的消息将被清理
    static constexpr int ACK_TIMEOUT_SECONDS = 300;
};

#endif // ACK_TRACKER_H
