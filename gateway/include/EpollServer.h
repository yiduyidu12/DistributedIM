// EpollServer - 基于Epoll的高性能网关服务器类
// 核心职责：管理TCP连接、处理客户端消息、路由跨网关消息、维护在线用户状态
// 采用Reactor模式，使用Epoll实现高效的IO多路复用

#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include "Connection.h"
#include "MessageHandler.h"
#include "RedisClient.h"

#include <sys/epoll.h>
#include <unordered_map>

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
  // 循环调用epoll_wait获取事件，依次处理：IO事件、离线消息推送、超时检测
  void loop();

  // 处理新客户端连接
  // 接受客户端连接，设置非阻塞模式，注册到Epoll，初始化Connection对象
  void handleAccept();

  // 处理客户端可读事件
  // 读取客户端数据到缓冲区，解析完整消息并调用handleMessage处理
  void handleRead(int client_fd);

  // 处理客户端可写事件（刷新写缓冲区）
  // 将写缓冲区中的数据发送给客户端，完成后取消EPOLLOUT注册
  void handleWrite(int client_fd);

  // 处理客户端消息
  // 根据消息类型分发到相应的处理器，支持LOGIN、WHO、JSON格式消息等
  void handleMessage(int client_fd, Connection &conn, const std::string &msg);

  // 断开客户端连接
  // 清理连接状态、从Epoll中移除、关闭socket、更新在线用户列表
  void disconnectClient(int client_fd);

  // 注册消息处理器
  // 注册chat、send、ping、who等消息类型的处理函数
  void registerHandlers();

  // 执行用户登录操作
  // 处理重复登录逻辑，调用Redis完成原子化登录
  bool performLogin(int client_fd, Connection &conn, const std::string &name);

  // 向客户端发送消息
  // 非阻塞发送，发送失败时数据存入写缓冲区并注册EPOLLOUT
  void sendToClient(int fd, const std::string &msg);

  // 广播消息给所有客户端
  // 参数 msg: 要广播的消息内容
  // 参数 exclude_fd: 排除的文件描述符，默认-1表示不排除任何客户端
  void broadcast(const std::string &msg, int exclude_fd = -1);

  // 根据用户名获取客户端文件描述符
  // 参数 username: 用户名
  // 返回值: 对应的文件描述符，未找到返回-1
  int getUserFd(const std::string &username);

  // 根据文件描述符获取用户名
  // 参数 fd: 文件描述符
  // 返回值: 对应的用户名，未找到返回空字符串
  std::string getUsername(int fd);

private:
  int port_;                    // 监听端口号
  int server_fd_;               // 服务器socket文件描述符
  int epfd_;                    // epoll实例文件描述符
  int gateway_id_;              // 当前网关的唯一标识，由进程ID生成

  std::unordered_map<int, Connection> connections_;  // 客户端连接管理，fd到Connection的映射
  std::unordered_map<std::string, int> user_map_;    // 用户名到fd的映射，用于快速查找

  MessageHandler handler_;      // 消息处理器，负责消息类型分发
  RedisClient redis_;           // Redis客户端，用于分布式用户管理和消息队列
};

#endif // EPOLL_SERVER_H