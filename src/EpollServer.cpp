// 核心功能：TCP连接管理、用户登录/登出、消息路由、心跳检测、分布式在线用户管理

#include "EpollServer.h"
#include "JsonParser.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 1024       // epoll_wait 一次能处理的最大事件数
#define TIMEOUT_SECONDS 300    // 客户端连接超时时间（秒），超过此时间无活动将被断开

// 将文件描述符设置为非阻塞模式
// fd: 文件描述符
// 返回值: 成功返回0，失败返回-1
static int setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 去除字符串末尾的空白字符（换行符、回车符、空格）
// s: 输入字符串（会被修改）
// 返回值: 去除末尾空白后的字符串
static std::string trimTrailingWhitespace(std::string s) {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

// EpollServer 构造函数
// port: 监听端口
EpollServer::EpollServer(int port) : port_(port), server_fd_(-1), epfd_(-1) {
  // 使用进程ID的后5位作为网关ID，用于分布式部署时区分不同网关
  gateway_id_ = static_cast<int>(getpid() % 100000);
}

// EpollServer 析构函数
// 负责清理所有资源：关闭客户端连接、关闭服务器socket、关闭epoll实例
EpollServer::~EpollServer() {
  // 关闭所有客户端连接
  for (auto const &[fd, conn] : connections_) {
    (void)conn;
    close(fd);
  }
  connections_.clear();

  if (server_fd_ != -1)
    close(server_fd_);
  if (epfd_ != -1)
    close(epfd_);
}

// 创建并初始化服务器监听socket
// 返回值: 成功返回socket文件描述符，失败返回-1
int EpollServer::createServerSocket() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    std::cerr << "Failed to create server socket" << std::endl;
    return -1;
  }

  // 设置SO_REUSEADDR选项，允许端口快速重用
  int opt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
    std::cerr << "Failed to set socket options" << std::endl;
    close(fd);
    return -1;
  }

  // 设置socket地址结构
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网络接口

  if (bind(fd, (sockaddr *)&addr, sizeof(addr)) == -1) {
    std::cerr << "Failed to bind server socket" << std::endl;
    close(fd);
    return -1;
  }

  // 开始监听，backlog为10
  if (listen(fd, 10) == -1) {
    std::cerr << "Failed to listen on server socket" << std::endl;
    close(fd);
    return -1;
  }

  // 设置为非阻塞模式
  if (setNonBlocking(fd) == -1) {
    std::cerr << "Failed to set server socket non-blocking" << std::endl;
    close(fd);
    return -1;
  }

  return fd;
}

// 注册消息处理器
// 注册各种消息类型的处理函数，包括：chat(群聊)、send(点对点)、ping(心跳)、who(在线用户)
void EpollServer::registerHandlers() {
  // 处理群聊消息
  handler_.registerHandler("chat", [this](const std::string &msg,
                                          const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }

    std::string content = getJsonString(msg, "msg");
    if (content.empty())
      return std::string();

    nlohmann::json sendJson{{"type", "chat"},
                            {"from", username},
                            {"msg", content},
                            {"timestamp", time(nullptr)}};
    std::string sendMsg = sendJson.dump() + "\n";

    // 给发送者自己回显消息
    int sender_fd = getUserFd(username);
    if (sender_fd != -1) {
      sendToClient(sender_fd, sendMsg);
    }

    // 广播给其他在线用户
    auto users = redis_.getAllOnlineUsers();
    for (auto &[name, gateway] : users) {
      if (name == username)
        continue;
      if (gateway == gateway_id_) {
        // 同网关：直接通过fd发送
        int fd = getUserFd(name);
        if (fd != -1)
          sendToClient(fd, sendMsg);
      } else {
        // 跨网关：通过Redis队列转发
        redis_.pushMessage(name, sendMsg);
      }
    }
    return std::string();
  });

  // 处理点对点消息
  handler_.registerHandler("send", [this](const std::string &msg,
                                          const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }

    std::string target = getJsonString(msg, "to");
    std::string content = getJsonString(msg, "msg");
    if (target.empty() || content.empty())
      return std::string();

    // 获取目标用户所在网关
    int target_gateway = redis_.getUserGateway(target);
    if (target_gateway == -1) {
      nlohmann::json err{{"type", "error"}, {"msg", "user not found"}};
      return err.dump() + "\n";
    }

    nlohmann::json sendJson{{"type", "send"},
                            {"from", username},
                            {"to", target},
                            {"msg", content},
                            {"timestamp", time(nullptr)}};
    std::string sendMsg = sendJson.dump() + "\n";

    // 根据目标网关决定发送方式
    if (target_gateway == gateway_id_) {
      // 同网关：直接发送
      int fd = getUserFd(target);
      if (fd != -1)
        sendToClient(fd, sendMsg);
      else
        redis_.pushMessage(target, sendMsg);
    } else {
      // 跨网关：通过Redis队列转发
      redis_.pushMessage(target, sendMsg);
    }
    return std::string();
  });

  // 处理心跳检测
  handler_.registerHandler("ping",
                           [](const std::string &, const std::string &) {
                             nlohmann::json reply{
                                 {"type", "pong"}, {"timestamp", time(nullptr)}};
                             return reply.dump() + "\n";
                           });

  // 处理在线用户查询
  handler_.registerHandler("who", [this](const std::string &,
                                         const std::string &) {
    auto users = redis_.getAllOnlineUsers();
    nlohmann::json reply{{"type", "who"}, {"users", nlohmann::json::array()}};
    for (auto &[name, gateway] : users) {
      (void)gateway;
      reply["users"].push_back(name);
    }
    return reply.dump() + "\n";
  });
}

// 启动网关服务器
// 步骤：1.连接Redis 2.创建服务器socket 3.创建epoll实例 4.注册消息处理器 5.进入事件循环
void EpollServer::start() {
  // 连接Redis（使用默认地址127.0.0.1:6379）
  if (!redis_.connect()) {
    std::cerr << "Failed to connect to Redis" << std::endl;
    return;
  }
  std::cout << "[INFO] Gateway ID: " << gateway_id_ << std::endl;

  // 创建服务器socket
  server_fd_ = createServerSocket();
  if (server_fd_ == -1) {
    std::cerr << "Failed to create server socket, exiting." << std::endl;
    return;
  }

  // 创建epoll实例
  epfd_ = epoll_create(1);
  if (epfd_ == -1) {
    std::cerr << "Failed to create epoll instance" << std::endl;
    close(server_fd_);
    server_fd_ = -1;
    return;
  }

  // 注册消息处理器
  registerHandlers();

  // 将服务器socket加入epoll监听
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = server_fd_;

  if (epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd_, &ev) == -1) {
    std::cerr << "Failed to add server socket to epoll" << std::endl;
    close(server_fd_);
    close(epfd_);
    server_fd_ = -1;
    epfd_ = -1;
    return;
  }

  std::cout << "Gateway Server started on port " << port_ << std::endl;
  loop();
}

// 主事件循环
// 循环执行：1.等待epoll事件 2.检查超时连接 3.处理跨网关消息 4.处理事件
void EpollServer::loop() {
  epoll_event events[MAX_EVENTS];
  while (true) {
    // 等待epoll事件，超时100ms以便定期检查超时连接
    int nfds = epoll_wait(epfd_, events, MAX_EVENTS, 100);
    if (nfds == -1) {
      if (errno == EINTR)
        continue;  // 被信号中断，继续循环
      std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
      continue;
    }

    time_t now = time(nullptr);

    // 检查连接超时
    for (auto it = connections_.begin(); it != connections_.end();) {
      if (now - it->second.last_active > TIMEOUT_SECONDS) {
        int fd = it->second.fd_;
        std::cout << "[TIMEOUT] fd=" << fd << " user=" << it->second.username
                  << std::endl;
        ++it;
        disconnectClient(fd);
      } else {
        ++it;
      }
    }

    // 处理跨网关消息：从Redis队列中取出消息发送给本地用户
    for (auto &[fd, conn] : connections_) {
      if (!conn.username.empty()) {
        auto msgs = redis_.popMessages(conn.username);
        for (const auto &msg : msgs)
          sendToClient(fd, msg);
      }
    }

    // 处理epoll事件
    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;
      if (fd == server_fd_)
        handleAccept();  // 新连接
      else
        handleRead(fd);  // 可读事件
    }
  }
}

// 处理新客户端连接
// 步骤：1.accept新连接 2.设置非阻塞 3.加入epoll 4.创建Connection对象
void EpollServer::handleAccept() {
  int client_fd = accept(server_fd_, nullptr, nullptr);
  if (client_fd == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      std::cerr << "Failed to accept new client connection" << std::endl;
    return;
  }

  // 设置客户端socket为非阻塞模式
  if (setNonBlocking(client_fd) == -1) {
    std::cerr << "Failed to set client socket non-blocking" << std::endl;
    close(client_fd);
    return;
  }

  // 将客户端socket加入epoll监听
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = client_fd;

  if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
    std::cerr << "Failed to add client socket to epoll" << std::endl;
    close(client_fd);
    return;
  }

  connections_.emplace(client_fd, Connection(client_fd));
  std::cout << "[INFO] New client: " << client_fd << std::endl;
}

// 断开客户端连接
// client_fd: 客户端文件描述符
void EpollServer::disconnectClient(int client_fd) {
  auto it = connections_.find(client_fd);
  if (it == connections_.end())
    return;

  Connection &conn = it->second;
  // 如果用户已登录，从Redis注销
  if (!conn.username.empty())
    redis_.userLogout(conn.username);

  // 从epoll中移除并关闭socket
  epoll_ctl(epfd_, EPOLL_CTL_DEL, client_fd, nullptr);
  close(client_fd);

  // 清理用户映射
  if (!conn.username.empty())
    user_map_.erase(conn.username);

  // 移除所有指向该fd的用户映射
  for (auto um = user_map_.begin(); um != user_map_.end();) {
    if (um->second == client_fd)
      um = user_map_.erase(um);
    else
      ++um;
  }

  // 移除连接对象
  connections_.erase(it);
}

// 执行用户登录操作
// client_fd: 客户端文件描述符
// conn: 连接对象引用
// name: 用户名
// 返回值: 登录成功返回true，失败返回false
bool EpollServer::performLogin(int client_fd, Connection &conn,
                               const std::string &name) {
  if (name.empty()) {
    nlohmann::json reply{{"type", "login"}, {"status", "fail"}};
    sendToClient(client_fd, reply.dump() + "\n");
    return false;
  }

  // 调用RedisClient进行登录（原子操作）
  if (redis_.userLogin(name, gateway_id_, client_fd)) {
    conn.username = name;
    conn.isLogin = true;
    user_map_[name] = client_fd;

    nlohmann::json reply{{"type", "login"}, {"status", "ok"}};
    sendToClient(client_fd, reply.dump() + "\n");
    std::cout << "[LOGIN] " << name << " (gateway=" << gateway_id_ << ")"
              << std::endl;
    return true;
  }

  nlohmann::json reply{{"type", "login"}, {"status", "fail"}};
  sendToClient(client_fd, reply.dump() + "\n");
  return false;
}

// 处理客户端消息
// 支持两种格式：命令行格式(LOGIN/WHO) 和 JSON格式({"type": "...", ...})
// client_fd: 客户端文件描述符
// conn: 连接对象引用
// rawMsg: 原始消息内容
void EpollServer::handleMessage(int client_fd, Connection &conn,
                                const std::string &rawMsg) {
  std::string msg = trimTrailingWhitespace(rawMsg);
  if (msg.empty())
    return;

  // 处理命令行格式的登录
  if (msg.rfind("LOGIN ", 0) == 0) {
    performLogin(client_fd, conn, trimTrailingWhitespace(msg.substr(6)));
    return;
  }

  // 处理命令行格式的WHO查询
  if (msg == "WHO" || msg.rfind("WHO ", 0) == 0) {
    std::string reply = handler_.handle("who", msg, conn.username);
    if (!reply.empty())
      sendToClient(client_fd, reply);
    return;
  }

  // 获取JSON消息类型
  std::string type = getJsonType(msg);

  // 处理JSON格式的登录
  if (type == "login") {
    performLogin(client_fd, conn, getJsonString(msg, "username"));
    return;
  }

  // 未登录用户只能发送ping消息
  if (!conn.isLogin && type != "ping") {
    nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
    sendToClient(client_fd, err.dump() + "\n");
    return;
  }

  // 未知消息类型
  if (type.empty()) {
    sendToClient(client_fd, "Unknown command\n");
    return;
  }

  // 调用消息处理器处理消息
  std::string reply = handler_.handle(type, msg, conn.username);
  if (!reply.empty())
    sendToClient(client_fd, reply);
}

// 处理客户端可读事件
// 从客户端读取数据，按行解析消息，并调用handleMessage处理
// client_fd: 客户端文件描述符
void EpollServer::handleRead(int client_fd) {
  auto it = connections_.find(client_fd);
  if (it == connections_.end())
    return;

  Connection &conn = it->second;
  char buf[1024];

  while (true) {
    int len = read(client_fd, buf, sizeof(buf));

    // 客户端正常断开
    if (len == 0) {
      std::cout << "[INFO] Client disconnected: " << client_fd << std::endl;
      disconnectClient(client_fd);
      return;
    }

    // 读取错误
    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;  // 非阻塞模式下没有更多数据
      std::cout << "[INFO] Client read error: " << client_fd << std::endl;
      disconnectClient(client_fd);
      return;
    }

    // 更新最后活动时间
    conn.last_active = time(nullptr);
    conn.read_buffer.append(buf, len);

    // 按行解析消息（以\n分隔）
    size_t pos = 0;
    while ((pos = conn.read_buffer.find('\n')) != std::string::npos) {
      std::string line = conn.read_buffer.substr(0, pos);
      conn.read_buffer.erase(0, pos + 1);
      handleMessage(client_fd, conn, line);
    }

    // 防止缓冲区溢出（最大64KB）
    if (conn.read_buffer.size() > 65536) {
      std::cerr << "[WARN] Read buffer overflow, closing fd=" << client_fd
                << std::endl;
      disconnectClient(client_fd);
      return;
    }
  }
}

// 向客户端发送消息
// 处理非阻塞写入，支持部分写入的情况
// fd: 客户端文件描述符
// msg: 要发送的消息内容
void EpollServer::sendToClient(int fd, const std::string &msg) {
  size_t total = 0;
  while (total < msg.size()) {
    ssize_t n = write(fd, msg.data() + total, msg.size() - total);
    if (n > 0) {
      total += static_cast<size_t>(n);
    } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;  // 非阻塞模式下暂时无法写入，稍后重试
    } else {
      std::cerr << "Error writing to client " << fd << ": " << strerror(errno)
                << std::endl;
      return;
    }
  }
  // 记录部分写入的情况
  if (total < msg.size()) {
    std::cerr << "Partial write to client " << fd << ". Wrote " << total
              << " of " << msg.size() << " bytes." << std::endl;
  }
}

// 广播消息给所有客户端
// msg: 要广播的消息内容
// exclude_fd: 排除的文件描述符（-1表示不排除）
void EpollServer::broadcast(const std::string &msg, int exclude_fd) {
  for (auto &[fd, conn] : connections_) {
    (void)conn;
    if (fd != exclude_fd)
      sendToClient(fd, msg);
  }
}

// 根据用户名获取客户端文件描述符
// username: 用户名
// 返回值: 成功返回文件描述符，失败返回-1
int EpollServer::getUserFd(const std::string &username) {
  auto it = user_map_.find(username);
  if (it != user_map_.end())
    return it->second;
  return -1;
}

// 根据文件描述符获取用户名
// fd: 文件描述符
// 返回值: 成功返回用户名，失败返回空字符串
std::string EpollServer::getUsername(int fd) {
  auto it = connections_.find(fd);
  if (it != connections_.end())
    return it->second.username;
  return "";
}