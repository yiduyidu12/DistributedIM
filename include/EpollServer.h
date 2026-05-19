#pragma once
#include <string>
#include <ctime>

class Connection {
public:
    int fd_;
    std::string username;
    bool isLogin;//判断是否登入
    time_t last_active;

    Connection(int fd)
        : fd_(fd), isLogin(false), last_active(time(nullptr)) {}
};
yidu@yidu:~/DistributedIM/gateway/include$ cat EpollServer.h
#pragma once

#include "Connection.h"
#include "MessageHandler.h"
#include "RedisClient.h"
#include <unordered_map>

class EpollServer {
public:
  EpollServer(int port);
  ~EpollServer();

  void start();
  // Getter方法
  RedisClient &getRedis() { return redis_; }
  int getGatewayId() const { return gateway_id_; }

private:
  // 初始化相关
  int createServerSocket();
  void loop();
  // 事件处理
  void handleAccept();
  void handleRead(int client_fd);
  // 消息发送
  void sendToClient(int fd, const std::string &msg);
  void broadcast(const std::string &msg, int exclude_fd = -1);
  // 用户查询
  int getUserFd(const std::string &username);
  std::string getUsername(int fd);

private:
  // 网络相关
  int port_;
  int server_fd_;
  int epfd_;
  // 用户数据（本地缓存
  std::unordered_map<int, Connection> connections_;
  std::unordered_map<std::string, int> user_map_;

  // 消息处理器
  MessageHandler handler_;
  // 分布式相关
  int gateway_id_;
  RedisClient redis_;
};