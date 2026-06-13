// RedisClient - Redis客户端封装类
// 提供分布式用户管理和消息队列功能
// 封装了用户登录/登出、在线状态查询、消息队列操作等原子化操作

#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <hiredis/hiredis.h>
#include <string>
#include <unordered_map>
#include <vector>

class RedisClient {
public:
  // 构造函数
  // 初始化Redis连接上下文为空
  RedisClient();

  // 析构函数
  // 断开Redis连接并释放资源
  ~RedisClient();

  // 连接Redis服务器
  // 参数 host: Redis服务器地址，默认127.0.0.1
  // 参数 port: Redis端口，默认6379
  // 返回值: 成功返回true，失败返回false
  bool connect(const std::string &host = "127.0.0.1", int port = 6379);

  // 断开Redis连接
  // 释放redisContext资源
  void disconnect();

  // 检查是否已连接
  // 返回值: 已连接返回true，未连接返回false
  bool isConnected() const;

  // 用户登录（原子操作）
  // 使用Lua脚本确保原子性，避免多网关并发登录竞态
  // 参数 username: 用户名
  // 参数 gateway_id: 网关ID
  // 参数 fd: 客户端文件描述符
  // 返回值: 1=成功, 0=用户已在线(拒绝), -1=错误
  int userLogin(const std::string &username, int gateway_id, int fd);

  // 用户登出（原子操作）
  // 使用MULTI/EXEC事务确保原子性
  // 参数 username: 用户名
  // 返回值: 成功返回true，失败返回false
  bool userLogout(const std::string &username);

  // 获取用户所在网关
  // 参数 username: 用户名
  // 返回值: 网关ID，失败返回-1
  int getUserGateway(const std::string &username);

  // 获取用户的客户端fd
  // 参数 username: 用户名
  // 返回值: 文件描述符，失败返回-1
  int getUserFd(const std::string &username);

  // 获取所有在线用户
  // 返回值: 用户名到网关ID的映射
  std::unordered_map<std::string, int> getAllOnlineUsers();

  // 推送消息到用户队列
  // 将消息存入用户的消息队列，并加入待处理集合
  // 参数 target: 目标用户名
  // 参数 msg: 消息内容
  // 返回值: 成功返回true，失败返回false
  bool pushMessage(const std::string &target, const std::string &msg);

  // 从用户队列中取出所有消息
  // 使用LPOP循环取出队列中所有消息
  // 参数 target: 目标用户名
  // 返回值: 消息列表
  std::vector<std::string> popMessages(const std::string &target);

  // 获取并清空有待收消息的用户集合
  // 使用Lua脚本原子化操作，避免竞态条件
  // 返回值: 有待收消息的用户名列表
  std::vector<std::string> drainPendingUsers();

private:
  redisContext *ctx_;  // Redis连接上下文，封装了Redis连接状态和错误信息
};

#endif // REDIS_CLIENT_H