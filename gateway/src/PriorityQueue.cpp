// PriorityQueue - 优先级消息队列
// 使用 std::priority_queue 实现三级优先级调度
// URGENT > NORMAL > BULK

#include "PriorityQueue.h"

// 构造函数
PriorityQueue::PriorityQueue() {}

// 向队列中添加消息
// 参数 payload: 消息内容
// 参数 target_fd: 目标客户端文件描述符
// 参数 priority: 消息优先级（默认 NORMAL）
void PriorityQueue::push(const std::string& payload, int target_fd,
                         MessagePriority priority) {
    PriorityMessage msg;
    msg.payload   = payload;
    msg.target_fd = target_fd;
    msg.priority  = priority;
    queue_.push(msg);
}

// 取出最高优先级消息
// 返回值: 优先级最高的消息，队列为空时 target_fd = -1
PriorityMessage PriorityQueue::pop() {
    if (queue_.empty()) {
        PriorityMessage empty;
        empty.target_fd = -1;
        return empty;
    }
    PriorityMessage top = queue_.top();
    queue_.pop();
    return top;
}

// 查看队列前端消息（不弹出）
// 返回值: 优先级最高的消息引用
const PriorityMessage& PriorityQueue::top() const {
    static PriorityMessage empty;
    if (queue_.empty()) {
        empty.target_fd = -1;
        return empty;
    }
    return queue_.top();
}

// 检查队列是否为空
// 返回值: 队列为空返回 true
bool PriorityQueue::empty() const {
    return queue_.empty();
}

// 获取队列大小
// 返回值: 队列中消息总数
size_t PriorityQueue::size() const {
    return queue_.size();
}

// 清空队列
void PriorityQueue::clear() {
    while (!queue_.empty()) {
        queue_.pop();
    }
}
