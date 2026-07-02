// Connection - 客户端连接类
// 存储单个客户端连接的状态信息，包括连接状态、缓冲区、登录状态等
// 支持 TCP 和 WebSocket 双协议，每连接独立的 ACK 追踪和限流

#ifndef CONNECTION_H
#define CONNECTION_H

#include "RateLimiter.h"
#include "WebSocketCodec.h"

#include <string>
#include <unordered_map>
#include <ctime>
#include <vector>
#include <cstdint>

// 连接协议类型
enum class Protocol {
    TCP,         // 原始 TCP（换行分割）
    WEBSOCKET,   // WebSocket（RFC 6455 帧协议）
    UNKNOWN,     // 尚未检测（等待客户端第一个数据包）
};

// WebSocket 连接子状态
struct WsState {
    WsFrameParseState parse_state;     // 帧解析状态机
    std::vector<uint8_t> raw_buffer;   // 原始字节缓冲区（用于帧解析）
    std::string          pending_upgrade_request; // 存储 HTTP Upgrade 请求（用于完成握手）

    bool   in_fragmented = false;      // 是否在分片重组模式中
    uint8_t fragment_opcode = 0;       // 分片首帧的操作码
    std::vector<uint8_t> reassembly_buf; // 分片重组缓冲区

    void reset() {
        parse_state.reset();
        raw_buffer.clear();
        pending_upgrade_request.clear();
        in_fragmented = false;
        fragment_opcode = 0;
        reassembly_buf.clear();
    }
};

// ACK 确认条目（每连接级别）
struct PendingAck {
    std::string msg_id;       // 消息唯一ID
    std::string original_msg; // 原始消息（用于重试）
    time_t      send_time;    // 发送时间戳
    int         retry_count;  // 已重试次数（最多3次）
    time_t      next_retry;   // 下次重试时间

    // 获取下次重试的指数退避间隔（秒）
    // 返回: 1 << retry_count = 1, 2, 4
    int retryDelay() const { return 1 << retry_count; }
};

class Connection {
public:
    // 构造函数
    // 参数 fd: 客户端文件描述符
    explicit Connection(int fd)
        : fd_(fd), isLogin(false), protocol(Protocol::UNKNOWN),
          last_active(time(nullptr)), device_id(""), rate_limiter(10.0, 20) {}

    int fd_;                    // 客户端文件描述符，用于标识唯一连接
    std::string username;       // 用户名，用户登录成功后设置，未登录时为空字符串
    bool isLogin;               // 是否已登录标志，true表示已登录，false表示未登录
    Protocol protocol;          // 当前连接使用的协议类型

    std::string read_buffer;    // 读取缓冲区（TCP模式下按换行符分割）
    std::string write_buffer;   // 写缓冲区，非阻塞模式下存储待发送的数据
    bool write_pending = false; // 是否已注册EPOLLOUT事件等待发送数据
    time_t last_active;         // 最后活动时间戳，用于超时检测和连接管理

    std::string device_id;      // 设备唯一标识（UUID），支持多设备登录

    // WebSocket 子状态（仅 WebSocket 连接使用）
    WsState ws;

    // ACK 追踪（每连接级别）
    // msg_id → PendingAck 映射
    std::unordered_map<std::string, PendingAck> pending_acks;

    // 令牌桶限流器（每连接独立限流）
    RateLimiter rate_limiter;
};

#endif // CONNECTION_H
