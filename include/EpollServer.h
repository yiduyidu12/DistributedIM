// EpollServer - 基于Epoll的高性能网关服务器类
// 核心职责：管理TCP连接、处理客户端消息、路由跨网关消息、维护在线用户状态

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
  // port: 服务器监听端口
  explicit EpollServer(int port);

  // 析构函数
  ~EpollServer();

  // 启动服务器
  void start();

private:
  // 创建服务器监听socket
  int createServerSocket();

  // 主事件循环
  void loop();

  // 处理新客户端连接
  void handleAccept();

  // 处理客户端可读事件
  void handleRead(int client_fd);

  // 处理客户端可写事件（刷新写缓冲区）
  void handleWrite(int client_fd);

  // 处理客户端消息
  void handleMessage(int client_fd, Connection &conn, const std::string &msg);

  // 断开客户端连接
  void disconnectClient(int client_fd);

  // 注册消息处理器
  void registerHandlers();

  // 执行用户登录操作
  bool performLogin(int client_fd, Connection &conn, const std::string &name);

  // 向客户端发送消息
  void sendToClient(int fd, const std::string &msg);

  // 广播消息给所有客户端
  void broadcast(const std::string &msg, int exclude_fd = -1);

  // 根据用户名获取客户端文件描述符
  int getUserFd(const std::string &username);

  // 根据文件描述符获取用户名
  std::string getUsername(int fd);

private:
  int port_;                    // 监听端口
  int server_fd_;               // 服务器socket文件描述符
  int epfd_;                    // epoll实例文件描述符
  int gateway_id_;              // 当前网关的唯一标识

  std::unordered_map<int, Connection> connections_;  // 客户端连接管理
  std::unordered_map<std::string, int> user_map_;    // 用户名到fd的映射

  MessageHandler handler_;      // 消息处理器
  RedisClient redis_;           // Redis客户端
};

#endif // EPOLL_SERVER_H