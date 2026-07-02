// RateLimiter - 令牌桶限流器
// 每连接独立的令牌桶限流，平滑突发流量
// 令牌以固定速率生成，桶满则丢弃多余令牌

#include "RateLimiter.h"
#include <algorithm>
#include <cmath>

// 构造函数
// 参数 rate: 令牌生成速率（tokens/秒），默认10.0
// 参数 capacity: 桶容量（最大令牌数），默认20（允许2秒突发）
RateLimiter::RateLimiter(double rate, int capacity)
    : rate_(rate), capacity_(capacity), tokens_(capacity), last_refill_(std::chrono::steady_clock::now()) {}

// 补充令牌
// 基于时间差计算应生成的令牌数，桶满则截断
void RateLimiter::refill() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_refill_).count() / 1e6;
    if (elapsed <= 0.0) return;

    double new_tokens = elapsed * rate_;
    tokens_ = std::min(static_cast<double>(capacity_), tokens_ + new_tokens);
    last_refill_ = now;
}

// 尝试消费一个令牌
// 先补充令牌再尝试消费
// 返回值: 有可用令牌返回true
bool RateLimiter::tryConsume() {
    refill();
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return true;
    }
    return false;
}

// 获取需要等待的秒数
// 返回值: 预计下次可获取令牌的等待秒数
double RateLimiter::retryAfterSeconds() const {
    // 非const版本的refill不方便，这里做近似计算
    double deficit = 1.0 - tokens_;
    if (deficit <= 0.0) return 0.0;
    return std::ceil(deficit / rate_);
}

// 更新令牌生成速率
// 参数 rate: 新的速率（tokens/秒）
void RateLimiter::setRate(double rate) {
    rate_ = rate;
}

// 获取当前速率
// 返回值: 令牌生成速率
double RateLimiter::getRate() const {
    return rate_;
}

// 获取当前可用令牌数
// 返回值: 当前桶中令牌数
double RateLimiter::availableTokens() const {
    return tokens_;
}

// 重置限流器状态
// 令牌数恢复到满桶
void RateLimiter::reset() {
    tokens_ = static_cast<double>(capacity_);
    last_refill_ = std::chrono::steady_clock::now();
}
