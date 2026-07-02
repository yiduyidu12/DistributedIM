// AckTracker - ACK 消息确认追踪器
// 追踪消息投递状态，使用指数退避策略自动重试
// 重试 3 次失败后移入死信队列

#include "AckTracker.h"
#include "Logger.h"
#include <algorithm>

// 构造函数
AckTracker::AckTracker() {}

// 注册一条待确认消息
// 参数 msg_id: 消息唯一ID（客户端生成）
// 参数 target_user: 目标用户名
// 参数 payload: 原始消息载荷
// 参数 target_fd: 目标客户端的文件描述符
void AckTracker::trackMessage(const std::string& msg_id, const std::string& target_user,
                              const std::string& payload, int target_fd) {
    // 加锁保护：避免并发写入 pending_acks_
    std::lock_guard<std::mutex> lock(mutex_);

    AckEntry entry;
    entry.msg_id      = msg_id;
    entry.target_user = target_user;
    entry.payload     = payload;
    entry.target_fd   = target_fd;
    entry.send_time   = time(nullptr);
    entry.retry_count = 0;
    entry.next_retry  = entry.send_time + 1;  // 首次重试等待 1 秒

    pending_acks_[msg_id] = entry;
    Logger::trace("[ACK] 追踪消息: msg_id={}, target={}", msg_id, target_user);
}

// 确认消息已被接收
// 参数 msg_id: 客户端回复 ACK 的消息ID
// 返回值: 确认成功返回true
bool AckTracker::acknowledgeMessage(const std::string& msg_id) {
    // 加锁保护：避免并发修改 pending_acks_
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pending_acks_.find(msg_id);
    if (it == pending_acks_.end()) {
        Logger::trace("[ACK] 未找到待确认消息: msg_id={}", msg_id);
        return false;
    }
    Logger::trace("[ACK] 消息已确认: msg_id={}, user={}, 耗时={}s",
                  msg_id, it->second.target_user,
                  time(nullptr) - it->second.send_time);
    pending_acks_.erase(it);
    return true;
}

// 获取需要重试的消息列表
// 检查所有待确认消息的 next_retry 时间是否已到
// 返回值: 需要立即重试的消息列表
std::vector<AckEntry> AckTracker::getPendingRetries() {
    // 加锁保护：避免遍历过程中其他线程修改 pending_acks_
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AckEntry> retries;
    time_t now = time(nullptr);

    for (auto& [msg_id, entry] : pending_acks_) {
        if (entry.retry_count < MAX_RETRIES && now >= entry.next_retry) {
            retries.push_back(entry);
        }
    }
    return retries;
}

// 标记重试已完成并更新下次重试时间
// 参数 msg_id: 消息ID
void AckTracker::markRetryDone(const std::string& msg_id) {
    // 加锁保护：避免并发修改 pending_acks_
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pending_acks_.find(msg_id);
    if (it == pending_acks_.end()) return;

    it->second.retry_count++;
    if (it->second.retry_count < MAX_RETRIES) {
        // 指数退避：next_retry = now + (1 << retry_count) 秒
        it->second.next_retry = time(nullptr) + it->second.retryDelaySeconds();
        Logger::trace("[ACK] 重试完成: msg_id={}, 重试次数={}/{}, 下次重试={}s后",
                      msg_id, it->second.retry_count, MAX_RETRIES,
                      it->second.retryDelaySeconds());
    } else {
        Logger::warn("[ACK] 消息投递失败（已达最大重试次数）: msg_id={}, target={}",
                     msg_id, it->second.target_user);
    }
}

// 获取超时放弃的消息（重试3次后仍失败）
// 这些消息将从追踪器中移除，调用方应将其移入死信队列
// 返回值: 已放弃的消息列表
std::vector<AckEntry> AckTracker::drainDeadLetters() {
    // 加锁保护：避免遍历和删除过程中其他线程修改 pending_acks_
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AckEntry> dead;
    auto it = pending_acks_.begin();
    while (it != pending_acks_.end()) {
        if (it->second.retry_count >= MAX_RETRIES) {
            dead.push_back(it->second);
            it = pending_acks_.erase(it);
        } else {
            ++it;
        }
    }
    if (!dead.empty()) {
        Logger::info("[ACK] {} 条消息移入死信队列", dead.size());
    }
    return dead;
}

// 获取待确认消息总数
// 返回值: 当前追踪中的消息数量
size_t AckTracker::pendingCount() const {
    // 加锁保护：避免并发访问 pending_acks_
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_acks_.size();
}

// 清理过期条目
// 移除超过 ACK_TIMEOUT_SECONDS 且无更多重试机会的消息
void AckTracker::purgeExpired() {
    // 加锁保护：避免遍历和删除过程中其他线程修改 pending_acks_
    std::lock_guard<std::mutex> lock(mutex_);

    time_t now = time(nullptr);
    auto it = pending_acks_.begin();
    while (it != pending_acks_.end()) {
        if (now - it->second.send_time > ACK_TIMEOUT_SECONDS &&
            it->second.retry_count >= MAX_RETRIES) {
            Logger::warn("[ACK] 清理过期条目: msg_id={}, target={}",
                         it->second.msg_id, it->second.target_user);
            it = pending_acks_.erase(it);
        } else {
            ++it;
        }
    }
}
