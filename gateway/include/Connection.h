// Connection - 客户端连接类
// 存储单个客户端连接的状态信息，包括连接状态、缓冲区、登录状态等

#ifndef CONNECTION_H
#define CONNECTION_H

#include <string>
#include <ctime>

class Connection {
public:
  // 构造函数
  // 参数 fd: 客户端文件描述符
  explicit Connection(int fd) : fd_(fd), isLogin(false), last_active(time(nullptr)) {}

  int fd_;              // 客户端文件描述符，用于标识唯一连接
  std::string username; // 用户名，用户登录成功后设置，未登录时为空字符串
  bool isLogin;         // 是否已登录标志，true表示已登录，false表示未登录
  std::string read_buffer;   // 读取缓冲区，存储从客户端读取但未处理的数据
  std::string write_buffer;  // 写缓冲区，非阻塞模式下存储待发送的数据
  bool write_pending = false; // 是否已注册EPOLLOUT事件等待发送数据
  time_t last_active;   // 最后活动时间戳，用于超时检测和连接管理
};

#endif // CONNECTION_H