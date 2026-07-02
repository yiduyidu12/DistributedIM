// PriorityQueue - 优先级消息队列
// 提供三级优先级消息调度，确保关键消息优先投递
// 优先级定义：
//   URGENT (0) - 系统消息、ACK 确认帧、心跳响应
//   NORMAL (1) - 普通聊天消息
//   BULK   (2) - 批量数据、文件通知、历史推送

#ifndef PRIORITY_QUEUE_H
#define PRIORITY_QUEUE_H

#include <queue>
#include <string>
#include <cstdint>

// 消息优先级枚举
enum class MessagePriority : uint8_t {
    URGENT = 0,  // 系统/ACK 级别，最高优先
    NORMAL = 1,  // 普通聊天消息
    BULK   = 2,  // 批量数据，最低优先
};

// 带优先级的消息条目
struct PriorityMessage {
    std::string      payload;   // 消息内容
    int              target_fd; // 目标文件描述符
    MessagePriority  priority;  // 消息优先级

    // 比较运算符：优先队列按优先级升序排列（数字越小越优先）
    bool operator<(const PriorityMessage& other) const {
        // priority 值越小越优先，所以用大于号反转
        return static_cast<uint8_t>(priority) > static_cast<uint8_t>(other.priority);
    }
};

// 优先级消息队列
// 内部使用 std::priority_queue，自动按优先级排序
class PriorityQueue {
public:
    // 构造函数
    PriorityQueue();

    // 向队列中添加消息
    // 参数 payload: 消息内容
    // 参数 target_fd: 目标客户端文件描述符
    // 参数 priority: 消息优先级
    void push(const std::string& payload, int target_fd,
              MessagePriority priority = MessagePriority::NORMAL);

    // 取出最高优先级消息
    // 返回值: 优先级最高的消息，队列为空时 target_fd = -1
    PriorityMessage pop();

    // 查看队列前端消息（不弹出）
    // 返回值: 优先级最高的消息，队列为空时 target_fd = -1
    const PriorityMessage& top() const;

    // 检查队列是否为空
    // 返回值: 队列为空返回 true
    bool empty() const;

    // 获取队列大小
    // 返回值: 队列中的消息数量
    size_t size() const;

    // 清空队列
    void clear();

private:
    std::priority_queue<PriorityMessage> queue_;
};

#endif // PRIORITY_QUEUE_H
