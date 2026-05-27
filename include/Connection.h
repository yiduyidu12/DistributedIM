// Connection - 客户端连接类
// 存储单个客户端连接的状态信息

#ifndef CONNECTION_H
#define CONNECTION_H

#include <string>
#include <ctime>

class Connection {
public:
  // 构造函数
  // fd: 客户端文件描述符
  explicit Connection(int fd) : fd_(fd), isLogin(false), last_active(time(nullptr)) {}

  int fd_;              // 客户端文件描述符
  std::string username; // 用户名（登录后设置）
  bool isLogin;         // 是否已登录
  std::string read_buffer;  // 读取缓冲区
  time_t last_active;   // 最后活动时间（用于超时检测）
};

#endif // CONNECTION_H