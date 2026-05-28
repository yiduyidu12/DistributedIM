// RedisClient - Redis客户端封装类
// 提供分布式用户管理和消息队列功能

#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <hiredis/hiredis.h>
#include <string>
#include <unordered_map>
#include <vector>

class RedisClient {
public:
  // 构造函数
  RedisClient();

  // 析构函数
  ~RedisClient();

  // 连接Redis服务器
  // host: Redis服务器地址（默认127.0.0.1）
  // port: Redis端口（默认6379）
  // 返回值: 成功返回true
  bool connect(const std::string &host = "127.0.0.1", int port = 6379);

  // 断开Redis连接
  void disconnect();

  // 用户登录（原子操作）
  // username: 用户名
  // gateway_id: 网关ID
  // fd: 客户端文件描述符
  // 返回值: 成功返回true
  bool userLogin(const std::string &username, int gateway_id, int fd);

  // 用户登出（原子操作）
  // username: 用户名
  // 返回值: 成功返回true
  bool userLogout(const std::string &username);

  // 获取用户所在网关
  // username: 用户名
  // 返回值: 网关ID，失败返回-1
  int getUserGateway(const std::string &username);

  // 获取用户的客户端fd
  // username: 用户名
  // 返回值: 文件描述符，失败返回-1
  int getUserFd(const std::string &username);

  // 获取所有在线用户
  // 返回值: 用户名到网关ID的映射
  std::unordered_map<std::string, int> getAllOnlineUsers();

  // 推送消息到用户队列
  // target: 目标用户名
  // msg: 消息内容
  // 返回值: 成功返回true
  bool pushMessage(const std::string &target, const std::string &msg);

  // 从用户队列中取出所有消息
  // target: 目标用户名
  // 返回值: 消息列表
  std::vector<std::string> popMessages(const std::string &target);

  // 获取并清空有待收消息的用户集合
  // 返回值: 有待收消息的用户名列表
  std::vector<std::string> drainPendingUsers();

private:
  redisContext *ctx_;  // Redis连接上下文
};

#endif // REDIS_CLIENT_H