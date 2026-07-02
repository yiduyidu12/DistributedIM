// RateLimiter - 令牌桶限流器
// 每连接独立的令牌桶限流，平滑突发流量
// 默认速率：10 msg/s，桶容量：20 tokens（允许2秒突发）
// 超过限流的消息返回错误 {"type":"error","msg":"rate limit exceeded","retry_after":5}

#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <cstdint>
#include <chrono>

class RateLimiter {
public:
    // 构造函数
    // 参数 rate: 令牌生成速率（tokens/秒），默认10
    // 参数 capacity: 桶容量（最大令牌数），默认20
    explicit RateLimiter(double rate = 10.0, int capacity = 20);

    // 尝试消费一个令牌
    // 先补充令牌（基于时间差），再尝试消费
    // 返回值: 有可用令牌返回true，否则返回false
    bool tryConsume();

    // 获取需要等待的秒数（限流时用于告知客户端）
    // 返回值: 预计下次可获取令牌的等待秒数
    double retryAfterSeconds() const;

    // 更新令牌生成速率
    // 参数 rate: 新的速率（tokens/秒）
    void setRate(double rate);

    // 获取当前速率
    // 返回值: 当前配置的令牌生成速率
    double getRate() const;

    // 获取当前可用令牌数
    // 返回值: 桶中当前令牌数
    double availableTokens() const;

    // 重置限流器状态
    void reset();

private:
    // 补充令牌（基于经过的时间计算）
    void refill();

    double rate_;                                       // 令牌生成速率（tokens/秒）
    int    capacity_;                                   // 桶容量上限
    double tokens_;                                     // 当前可用令牌数
    std::chrono::steady_clock::time_point last_refill_; // 上次补充时间点（使用steady_clock避免系统时间调整影响）
};

#endif // RATE_LIMITER_H
