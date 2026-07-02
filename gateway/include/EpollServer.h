// EpollServer - 基于Epoll的高性能网关服务器类
// 核心职责：管理TCP/WebSocket连接、处理客户端消息、路由跨网关消息、维护在线用户状态
// 采用Reactor模式，使用Epoll实现高效的IO多路复用
// 支持多设备登录、群组聊天、ACK可靠投递、优先队列、令牌桶限流

#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include "Connection.h"
#include "MessageHandler.h"
#include "RedisClient.h"
#include "GroupManager.h"
#include "AckTracker.h"
#include "PriorityQueue.h"
#include "GatewayRegistry.h"
#include "Metrics.h"

#ifdef AI_SERVICE_ENABLED
#include "AIServiceClient.h"
#include "AuditLogger.h"
#endif

#include <sys/epoll.h>
#include <unordered_map>
#include <vector>

class EpollServer {
public:
    // 构造函数
    // 参数 port: 服务器监听端口
    explicit EpollServer(int port);

    // 析构函数
    // 负责清理所有资源，包括关闭socket、断开Redis连接等
    ~EpollServer();

    // 启动服务器
    // 依次执行：连接Redis、创建socket、初始化Epoll、注册处理器、进入事件循环
    void start();

private:
    // 创建服务器监听socket
    // 设置SO_REUSEADDR和SO_REUSEPORT选项，绑定端口并监听
    // 返回值: 成功返回socket文件描述符，失败返回-1
    int createServerSocket();

    // 主事件循环
    // 循环调用epoll_wait获取事件，依次处理：IO事件、ACK重试、离线消息推送、超时检测
    void loop();

    // 处理新客户端连接
    // 接受客户端连接，设置非阻塞模式，注册到Epoll，初始化Connection对象
    void handleAccept();

    // 处理客户端可读事件
    // 支持 TCP 和 WebSocket 双协议的消息解析
    void handleRead(int client_fd);

    // 读取并处理 TCP 协议消息（按换行符分割）
    void handleTcpRead(int client_fd, Connection& conn);

    // 读取并处理 WebSocket 协议消息（帧解析状态机）
    void handleWsRead(int client_fd, Connection& conn);

    // 处理 WebSocket 控制帧（Ping/Pong/Close 在帧解析层内联处理）
    void handleWsControlFrame(int client_fd, Connection& conn, const WsFrame& frame);

    // 检测 HTTP Upgrade 请求并完成 WebSocket 握手
    bool tryWebSocketUpgrade(int client_fd, Connection& conn, const std::string& data);

    // 处理客户端可写事件（刷新写缓冲区）
    void handleWrite(int client_fd);

    // 处理客户端消息
    // 根据消息类型分发到相应的处理器
    void handleMessage(int client_fd, Connection &conn, const std::string &msg);

    // 断开客户端连接
    void disconnectClient(int client_fd);

    // 注册消息处理器
    // 注册chat、send、ping、who、group_*、ack等消息类型的处理函数
    void registerHandlers();

    // 执行用户登录操作
    // 支持多设备：user_map_ 存储 username → vector<fd> 映射
    bool performLogin(int client_fd, Connection &conn, const std::string &name);

    // 向客户端发送消息
    // 自动根据协议类型选择发送方式（TCP原样 / WebSocket帧包装）
    void sendToClient(int fd, const std::string &msg);

    // 向用户的所有设备发送消息（多设备广播）
    void sendToUser(const std::string& username, const std::string& msg);

    // 广播消息给所有客户端
    void broadcast(const std::string &msg, int exclude_fd = -1);

    // 根据用户名获取客户端文件描述符列表
    // 支持多设备：返回用户所有在线设备的fd列表
    std::vector<int> getUserFds(const std::string &username);

    // 根据用户名获取主设备fd（第一个在线设备）
    int getUserFd(const std::string &username);

    // 根据文件描述符获取用户名
    std::string getUsername(int fd);

    // 构建 HTTP /metrics 响应（供 Prometheus 抓取）
    std::string buildHttpMetricsResponse() const;

    // ACK 重试检查（在事件循环中调用）
    void checkAckRetries();

    // 离线消息推送（从Redis拉取并投递）
    void pushOfflineMessages();

    // 超时检测
    void checkTimeouts();

private:
    int port_;                    // 监听端口号
    int server_fd_;               // 服务器socket文件描述符
    int epfd_;                    // epoll实例文件描述符
    int gateway_id_;              // 当前网关的唯一标识

    // 客户端连接管理：fd → Connection 的映射
    std::unordered_map<int, Connection> connections_;

    // 用户名到fd列表的映射（支持多设备，一个用户多个fd）
    std::unordered_map<std::string, std::vector<int>> user_map_;

    MessageHandler handler_;      // 消息处理器，负责消息类型分发
    RedisClient    redis_;        // Redis客户端，用于分布式用户管理和消息队列
    GroupManager   group_mgr_;    // 群组管理器
    AckTracker     ack_tracker_;  // ACK 确认追踪器
    PriorityQueue  priority_q_;   // 优先级消息队列
    GatewayRegistry gateway_registry_; // 网关自动发现与负载均衡

#ifdef AI_SERVICE_ENABLED
    AIServiceClient ai_client_;   // AI 服务客户端（非阻塞 HTTP，集成到 epoll）
#endif
};

#endif // EPOLL_SERVER_H
