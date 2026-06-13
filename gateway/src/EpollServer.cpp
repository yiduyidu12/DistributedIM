// EpollServer - 基于Epoll的高性能网关服务器类
// 核心职责：管理TCP连接、处理客户端消息、路由跨网关消息、维护在线用户状态
// 采用Reactor模式，使用Epoll实现高效的IO多路复用
// 支持分布式部署，通过Redis实现跨网关消息路由

#include "EpollServer.h"
#include "JsonParser.h"
#include "Logger.h"
#include "Config.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// 单次epoll_wait最多处理的事件数
#define MAX_EVENTS 1024

// 设置文件描述符为非阻塞模式
// 参数 fd: 要设置的文件描述符
// 返回值: 成功返回0，失败返回-1
static int setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 去除字符串尾部的空白字符（换行符、回车符、空格）
// 参数 s: 要处理的字符串
// 返回值: 处理后的字符串
static std::string trimTrailingWhitespace(std::string s) {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

// 构造函数
// 参数 port: 服务器监听端口
EpollServer::EpollServer(int port) : port_(port), server_fd_(-1), epfd_(-1) {
  // 组合PID和端口号生成网关ID，降低跨机器网关ID碰撞概率
  gateway_id_ = static_cast<int>((getpid() * 31 + port_) % 100000);
}

// 析构函数
// 清理所有资源：关闭所有客户端连接、断开Redis、关闭服务器socket和epoll实例
EpollServer::~EpollServer() {
  // 关闭所有客户端连接
  for (auto const &[fd, conn] : connections_) {
    (void)conn;
    close(fd);
  }
  connections_.clear();
  
  // 断开Redis连接
  redis_.disconnect();
  
  // 关闭服务器socket
  if (server_fd_ != -1)
    close(server_fd_);
  
  // 关闭epoll实例
  if (epfd_ != -1)
    close(epfd_);
}

// 创建服务器监听socket
// 设置SO_REUSEADDR和SO_REUSEPORT选项，绑定端口并监听
// 返回值: 成功返回socket文件描述符，失败返回-1
int EpollServer::createServerSocket() {
  // 创建TCP socket
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    Logger::error("Failed to create server socket: {}", strerror(errno));
    return -1;
  }

  // 设置地址复用和端口复用选项
  int opt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
    Logger::warn("Failed to set SO_REUSEADDR: {}", strerror(errno));
  }
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
    Logger::warn("Failed to set SO_REUSEPORT: {}", strerror(errno));
  }

  // 配置socket地址结构
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = INADDR_ANY;

  // 绑定端口
  if (bind(fd, (sockaddr *)&addr, sizeof(addr)) == -1) {
    Logger::error("Failed to bind server socket to port {}: {}", port_, strerror(errno));
    if (errno == EADDRINUSE) {
      Logger::error("Port {} is already in use. Try another port or wait for it to be released.", port_);
    }
    close(fd);
    return -1;
  }

  // 开始监听
  if (listen(fd, 10) == -1) {
    Logger::error("Failed to listen on server socket");
    close(fd);
    return -1;
  }

  // 设置为非阻塞模式
  if (setNonBlocking(fd) == -1) {
    Logger::error("Failed to set server socket non-blocking");
    close(fd);
    return -1;
  }

  return fd;
}

// 注册消息处理器
// 注册chat、send、ping、who等消息类型的处理函数
void EpollServer::registerHandlers() {
  // 注册群聊消息处理器
  handler_.registerHandler("chat", [this](const std::string &msg,
                                          const std::string &username) {
    Logger::debug("[Handler] chat message received from user='{}'", 
                  username.empty() ? "(not logged in)" : username);
    
    // 检查用户是否已登录
    if (username.empty()) {
      Logger::warn("[Chat] Unauthorized chat attempt from non-logged-in user");
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }

    // 提取消息内容
    std::string content = getJsonString(msg, "msg");
    if (content.empty()) {
      Logger::warn("[Chat] Empty message content from user='{}'", username);
      return std::string();
    }
    Logger::trace("[Chat] Message content length={} from user='{}'", content.size(), username);

    // 构建群聊消息JSON
    nlohmann::json sendJson{{"type", "chat"},
                            {"from", username},
                            {"msg", content},
                            {"timestamp", time(nullptr)}};
    std::string sendMsg = sendJson.dump() + "\n";

    // 发送给自己
    int sender_fd = getUserFd(username);
    if (sender_fd != -1) {
      Logger::trace("[Chat] Sending chat message to self (fd={})", sender_fd);
      sendToClient(sender_fd, sendMsg);
    } else {
      Logger::warn("[Chat] Cannot find sender fd for user='{}'", username);
    }

    // 获取所有在线用户并广播
    auto users = redis_.getAllOnlineUsers();
    Logger::debug("[Chat] Broadcasting to {} online users", users.size());
    
    int local_count = 0, remote_count = 0;
    for (auto &[name, gateway] : users) {
      // 跳过自己
      if (name == username)
        continue;
      
      // 判断用户是否在本地网关
      if (gateway == gateway_id_) {
        int fd = getUserFd(name);
        if (fd != -1) {
          Logger::trace("[Chat] Sending to local user='{}' (fd={})", name, fd);
          sendToClient(fd, sendMsg);
          local_count++;
        } else {
          Logger::warn("[Chat] User '{}' is local but fd not found", name);
        }
      } else {
        // 远程用户，通过Redis消息队列转发
        Logger::trace("[Chat] Queueing for remote user='{}' (gateway={})", name, gateway);
        redis_.pushMessage(name, sendMsg);
        remote_count++;
      }
    }
    Logger::info("[Chat] Broadcast completed: {} local, {} remote users", local_count, remote_count);
    return std::string();
  });

  // 注册私聊消息处理器
  handler_.registerHandler("send", [this](const std::string &msg,
                                          const std::string &username) {
    Logger::debug("[Handler] private message received from user='{}'", 
                  username.empty() ? "(not logged in)" : username);
    
    // 检查用户是否已登录
    if (username.empty()) {
      Logger::warn("[Send] Unauthorized private message attempt from non-logged-in user");
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }

    // 提取目标用户和消息内容
    std::string target = getJsonString(msg, "to");
    std::string content = getJsonString(msg, "msg");
    
    if (target.empty()) {
      Logger::warn("[Send] Empty target user from user='{}'", username);
      return std::string();
    }
    if (content.empty()) {
      Logger::warn("[Send] Empty message content from user='{}' to '{}'", username, target);
      return std::string();
    }
    Logger::trace("[Send] Private message: from='{}', to='{}', length={}", username, target, content.size());

    // 查询目标用户所在网关
    int target_gateway = redis_.getUserGateway(target);
    Logger::debug("[Send] Target user='{}' gateway lookup result: {}", target, target_gateway);
    
    if (target_gateway == -1) {
      Logger::warn("[Send] Target user '{}' not found or offline", target);
      nlohmann::json err{{"type", "error"}, {"msg", "user not found"}};
      return err.dump() + "\n";
    }

    // 构建私聊消息JSON
    nlohmann::json sendJson{{"type", "send"},
                            {"from", username},
                            {"to", target},
                            {"msg", content},
                            {"timestamp", time(nullptr)}};
    std::string sendMsg = sendJson.dump() + "\n";

    // 判断目标用户是否在本地网关
    if (target_gateway == gateway_id_) {
      int fd = getUserFd(target);
      if (fd != -1) {
        Logger::trace("[Send] Delivering locally to user='{}' (fd={})", target, fd);
        sendToClient(fd, sendMsg);
      } else {
        Logger::warn("[Send] User '{}' is local but fd not found, queuing message", target);
        redis_.pushMessage(target, sendMsg);
      }
    } else {
      // 远程网关，通过Redis消息队列转发
      Logger::trace("[Send] Forwarding to remote gateway {} for user='{}'", target_gateway, target);
      redis_.pushMessage(target, sendMsg);
    }
    return std::string();
  });

  // 注册心跳处理器（ping/pong）
  handler_.registerHandler("ping",
                           [this](const std::string &, const std::string &username) {
                             (void)username;
                             nlohmann::json reply{
                                 {"type", "pong"}, {"timestamp", time(nullptr)}};
                             return reply.dump() + "\n";
                           });

  // 注册在线用户查询处理器
  handler_.registerHandler("who", [this](const std::string &,
                                         const std::string &username) {
    Logger::debug("[Handler] who request received from user='{}'", 
                  username.empty() ? "(not logged in)" : username);
    
    // 获取所有在线用户
    auto users = redis_.getAllOnlineUsers();
    Logger::debug("[Who] Retrieved {} online users from Redis", users.size());
    
    // 构建响应JSON
    nlohmann::json reply{{"type", "who"}, {"users", nlohmann::json::array()}};
    for (auto &[name, gateway] : users) {
      (void)gateway;
      reply["users"].push_back(name);
      Logger::trace("[Who] Online user: '{}' (gateway={})", name, gateway);
    }
    
    std::string response = reply.dump() + "\n";
    Logger::trace("[Who] Built response, length={}", response.size());
    return response;
  });
}

// 启动服务器
// 依次执行：连接Redis、创建socket、初始化Epoll、注册处理器、进入事件循环
void EpollServer::start() {
  Logger::info("Starting EpollServer on port {}", port_);
  
  // 步骤1：连接Redis
  if (!redis_.connect(Config::redis().host, Config::redis().port)) {
    Logger::error("[Redis] Failed to connect to {}:{}", Config::redis().host, Config::redis().port);
    return;  // 此时还没有分配资源，无需清理
  }
  Logger::info("[Redis] Connected successfully to {}:{}", Config::redis().host, Config::redis().port);
  Logger::info("[Gateway] ID: {}", gateway_id_);

  // 步骤2：创建服务器socket
  server_fd_ = createServerSocket();
  if (server_fd_ == -1) {
    Logger::critical("[Server] Failed to create server socket on port {}, exiting.", port_);
    redis_.disconnect();  // 清理Redis连接
    return;
  }
  Logger::debug("[Socket] Created server socket fd={}", server_fd_);

  // 步骤3：创建epoll实例
  epfd_ = epoll_create(1);
  if (epfd_ == -1) {
    Logger::critical("[Epoll] Failed to create epoll instance: {}", strerror(errno));
    close(server_fd_);
    server_fd_ = -1;
    redis_.disconnect();  // 清理Redis连接
    return;
  }
  Logger::debug("[Epoll] Created epoll instance fd={}", epfd_);

  // 步骤4：注册消息处理器
  registerHandlers();
  Logger::debug("[Handler] Registered {} handlers", 4);

  // 步骤5：将服务器socket注册到epoll
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = server_fd_;

  if (epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd_, &ev) == -1) {
    Logger::critical("[Epoll] Failed to add server socket to epoll: {}", strerror(errno));
    close(server_fd_);
    close(epfd_);
    server_fd_ = -1;
    epfd_ = -1;
    redis_.disconnect();  // 清理Redis连接
    return;
  }

  Logger::info("[Server] Gateway Server started on port {} (fd={})", port_, server_fd_);
  Logger::info("[Server] Max connections: {}, Timeout: {}s", 
               Config::server().max_connections, Config::server().timeout_seconds);
  
  // 步骤6：进入主事件循环
  loop();
}

// 主事件循环
// 循环调用epoll_wait获取事件，依次处理：IO事件、离线消息推送、超时检测
void EpollServer::loop() {
  epoll_event events[MAX_EVENTS];
  while (true) {
    // 等待事件，超时时间100ms（用于定期检测超时）
    int nfds = epoll_wait(epfd_, events, MAX_EVENTS, 100);
    if (nfds == -1) {
      if (errno == EINTR)
        continue;  // 被信号中断，继续循环
      Logger::error("epoll_wait error: {}", strerror(errno));
      continue;
    }

    // ① 先处理本轮所有事件，让活跃连接有机会更新last_active
    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;
      if (fd == server_fd_) {
        // 新客户端连接
        handleAccept();
      } else {
        // 客户端IO事件
        if (events[i].events & EPOLLIN)
          handleRead(fd);
        if (events[i].events & EPOLLOUT)
          handleWrite(fd);
      }
    }

    // ② 推送Redis中的离线消息
    auto pending = redis_.drainPendingUsers();
    for (const auto &name : pending) {
      int fd = getUserFd(name);
      if (fd == -1)
        continue;
      auto msgs = redis_.popMessages(name);
      for (const auto &msg : msgs)
        sendToClient(fd, msg);
    }

    // ③ 最后检查超时 —— 此时刚处理完事件的连接last_active已刷新，不会被误杀
    time_t now = time(nullptr);
    std::vector<int> timed_out_fds;
    for (const auto& [fd, conn] : connections_) {
      if (now - conn.last_active > Config::server().timeout_seconds) {
        Logger::connectionTimeout(fd, conn.username);
        timed_out_fds.push_back(fd);
      }
    }
    // 批量断开超时连接
    for (int fd : timed_out_fds) {
      disconnectClient(fd);
    }
  }
}

// 处理新客户端连接
// 接受客户端连接，设置非阻塞模式，注册到Epoll，初始化Connection对象
void EpollServer::handleAccept() {
  sockaddr_in client_addr{};
  socklen_t addr_len = sizeof(client_addr);
  
  // 接受新连接
  int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &addr_len);
  if (client_fd == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      Logger::warn("[Accept] Failed to accept new client: {}", strerror(errno));
    return;
  }
  
  // 获取客户端IP地址
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

  // 设置非阻塞模式
  if (setNonBlocking(client_fd) == -1) {
    Logger::warn("[Socket] Failed to set client socket non-blocking for fd={}", client_fd);
    close(client_fd);
    return;
  }

  // 注册到epoll
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = client_fd;

  if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
    Logger::warn("[Epoll] Failed to add client socket fd={} to epoll: {}", client_fd, strerror(errno));
    close(client_fd);
    return;
  }

  // 创建连接对象
  connections_.emplace(client_fd, Connection(client_fd));
  Logger::clientConnected(client_fd, ip, ntohs(client_addr.sin_port), connections_.size());
}

// 断开客户端连接
// 清理连接状态、从Epoll中移除、关闭socket、更新在线用户列表
void EpollServer::disconnectClient(int client_fd) {
  auto it = connections_.find(client_fd);
  if (it == connections_.end()) {
    Logger::warn("[Client] disconnectClient called for unknown fd={}", client_fd);
    return;
  }

  Connection &conn = it->second;
  std::string username = conn.username;

  // 清空写缓冲区
  conn.write_buffer.clear();
  conn.write_pending = false;

  // 如果用户已登录，执行登出操作
  if (!username.empty()) {
    if (redis_.userLogout(username)) {
      Logger::userLogout(client_fd, username);
    } else {
      Logger::userLogoutFailed(client_fd, username);
    }
  }

  // 从epoll中移除
  epoll_ctl(epfd_, EPOLL_CTL_DEL, client_fd, nullptr);
  
  // 关闭socket
  close(client_fd);

  // 移除用户名映射
  if (!username.empty())
    user_map_.erase(username);

  // 移除连接对象
  connections_.erase(it);
  Logger::clientDisconnected(client_fd, username, connections_.size());
}

// 执行用户登录操作
// 处理重复登录逻辑，调用Redis完成原子化登录
// 参数 client_fd: 客户端文件描述符
// 参数 conn: 连接对象引用
// 参数 name: 用户名
// 返回值: 成功返回true，失败返回false
bool EpollServer::performLogin(int client_fd, Connection &conn,
                               const std::string &name) {
  Logger::debug("[Login] Login attempt started for fd={}, username='{}'", client_fd, name);
  
  // 检查用户名是否为空
  if (name.empty()) {
    Logger::userLoginFailed(client_fd, name, "empty username");
    nlohmann::json reply{{"type", "login"}, {"status", "fail"}, {"msg", "username cannot be empty"}};
    sendToClient(client_fd, reply.dump() + "\n");
    Logger::trace("[Login] Sent error response for empty username to fd={}", client_fd);
    return false;
  }

  // 检查重复登录 —— 仅记录旧fd，暂不破坏旧连接状态
  // 等Redis登录成功后再清理旧连接，避免Redis失败导致两端都损坏
  int old_fd = -1;
  auto old_it = user_map_.find(name);
  if (old_it != user_map_.end() && old_it->second != client_fd) {
    old_fd = old_it->second;
    Logger::debug("[Login] Detected existing session for user='{}' on old_fd={}, will replace on success", name, old_fd);
  }

  // 执行Redis用户登录
  Logger::debug("[Login] Attempting Redis login for user='{}' (fd={}, gateway={})", 
                name, client_fd, gateway_id_);
  
  int login_result = redis_.userLogin(name, gateway_id_, client_fd);
  if (login_result == 1) {
    Logger::debug("[Login] Redis login succeeded for user='{}'", name);

    // Redis成功后，安全地清理旧连接
    if (old_fd != -1) {
      auto old_conn = connections_.find(old_fd);
      if (old_conn != connections_.end()) {
        old_conn->second.username.clear();
        old_conn->second.isLogin = false;
        Logger::duplicateLogin(name, old_fd, client_fd);
      }
      user_map_.erase(name);  // 移除旧的username -> fd映射
      Logger::debug("[Login] Cleaned up previous session for user='{}'", name);
    }

    // 更新新连接状态
    conn.username = name;
    conn.isLogin = true;
    user_map_[name] = client_fd;
    conn.last_active = time(nullptr);
    Logger::trace("[Login] Connection state updated: fd={}, username='{}', isLogin=true",
                  client_fd, name);

    // 发送成功响应
    nlohmann::json reply{{"type", "login"}, {"status", "ok"}};
    sendToClient(client_fd, reply.dump() + "\n");
    Logger::trace("[Login] Sent success response to fd={}", client_fd);

    // 记录登录成功日志
    Logger::userLogin(client_fd, name, gateway_id_, user_map_.size());
    return true;
  }

  // 登录失败处理 —— 旧连接状态未被修改，仍然完好
  if (login_result == 0) {
    // 用户已在其他网关或本网关其他连接上在线
    Logger::userLoginFailed(client_fd, name, "user already online");
    nlohmann::json reply{{"type", "login"}, {"status", "fail"}, {"msg", "user already logged in"}};
    sendToClient(client_fd, reply.dump() + "\n");
  } else {
    // Redis错误
    Logger::userLoginFailed(client_fd, name, "Redis login failed");
    nlohmann::json reply{{"type", "login"}, {"status", "fail"}, {"msg", "login failed"}};
    sendToClient(client_fd, reply.dump() + "\n");
  }
  Logger::trace("[Login] Sent failure response to fd={}", client_fd);
  return false;
}

// 处理客户端消息
// 根据消息类型分发到相应的处理器，支持LOGIN、WHO、JSON格式消息等
// 参数 client_fd: 客户端文件描述符
// 参数 conn: 连接对象引用
// 参数 rawMsg: 原始消息内容
void EpollServer::handleMessage(int client_fd, Connection &conn,
                                const std::string &rawMsg) {
  std::string msg = trimTrailingWhitespace(rawMsg);
  if (msg.empty()) {
    Logger::trace("[Message] Empty message received from fd={}", client_fd);
    return;
  }

  std::string type = getJsonType(msg);
  Logger::messageReceived(client_fd, conn.username, msg.size(), type);

  // 处理纯文本LOGIN命令
  if (msg.rfind("LOGIN ", 0) == 0) {
    std::string username = trimTrailingWhitespace(msg.substr(6));
    Logger::debug("[Message] Plain text login command: fd={}, username='{}'", client_fd, username);
    performLogin(client_fd, conn, username);
    return;
  }

  // 处理纯文本WHO命令
  if (msg == "WHO" || msg.rfind("WHO ", 0) == 0) {
    Logger::trace("[Message] WHO command received from fd={}", client_fd);
    std::string reply = handler_.handle("who", msg, conn.username);
    if (!reply.empty())
      sendToClient(client_fd, reply);
    return;
  }

  // 处理JSON格式登录请求
  if (type == "login") {
    std::string username = getJsonString(msg, "username");
    Logger::debug("[Message] JSON login request: fd={}, username='{}'", client_fd, username);
    performLogin(client_fd, conn, username);
    return;
  }

  // 心跳处理 —— 提前拦截以便用正确的fd记录日志
  if (type == "ping") {
    Logger::heartbeatReceived(client_fd, conn.username);
    std::string reply = handler_.handle("ping", msg, conn.username);
    if (!reply.empty()) {
      sendToClient(client_fd, reply);
      Logger::heartbeatSent(client_fd);
    }
    return;
  }

  // 检查用户是否已登录（心跳除外）
  if (!conn.isLogin) {
    Logger::unauthorizedAccess(client_fd, type);
    nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
    sendToClient(client_fd, err.dump() + "\n");
    return;
  }

  // 未知消息类型
  if (type.empty()) {
    Logger::unknownCommand(client_fd, msg);
    sendToClient(client_fd, "Unknown command\n");
    return;
  }

  // 分发到对应的消息处理器
  std::string reply = handler_.handle(type, msg, conn.username);
  if (!reply.empty())
    sendToClient(client_fd, reply);
}

// 处理客户端可读事件
// 读取客户端数据到缓冲区，解析完整消息并调用handleMessage处理
// 参数 client_fd: 客户端文件描述符
void EpollServer::handleRead(int client_fd) {
  auto it = connections_.find(client_fd);
  if (it == connections_.end()) {
    Logger::warn("[Read] Connection not found for fd={}", client_fd);
    return;
  }

  Connection &conn = it->second;
  char buf[1024];

  // 循环读取所有可用数据
  while (true) {
    int len = read(client_fd, buf, sizeof(buf));

    // 客户端关闭连接
    if (len == 0) {
      Logger::info("[Read] Client closed connection: fd={}, user={}", client_fd, conn.username.empty() ? "(unknown)" : conn.username);
      disconnectClient(client_fd);
      return;
    }

    // 读取错误
    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;  // 非阻塞模式下没有更多数据
      Logger::error("[Read] Error reading from fd={}: {}", client_fd, strerror(errno));
      disconnectClient(client_fd);
      return;
    }

    // 更新活动时间并追加到缓冲区
    conn.last_active = time(nullptr);
    conn.read_buffer.append(buf, len);
    Logger::trace("[Read] fd={} received {} bytes, buffer_size={}", client_fd, len, conn.read_buffer.size());

    // 解析完整消息（按换行符分割）
    size_t pos = 0;
    while ((pos = conn.read_buffer.find('\n')) != std::string::npos) {
      std::string line = conn.read_buffer.substr(0, pos);
      conn.read_buffer.erase(0, pos + 1);
      handleMessage(client_fd, conn, line);
    }

    // 缓冲区溢出检测
    if (conn.read_buffer.size() > 65536) {
      Logger::error("[Read] Buffer overflow for fd={}, size={}, closing connection", 
                    client_fd, conn.read_buffer.size());
      disconnectClient(client_fd);
      return;
    }
  }
}

// 向客户端发送消息
// 非阻塞发送，发送失败时数据存入写缓冲区并注册EPOLLOUT
// 参数 fd: 客户端文件描述符
// 参数 msg: 要发送的消息内容
void EpollServer::sendToClient(int fd, const std::string &msg) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) {
    Logger::warn("[Send] Connection not found for fd={}", fd);
    return;
  }

  Connection &conn = it->second;
  
  // 如果写缓冲区不为空，追加到缓冲区
  if (!conn.write_buffer.empty()) {
    conn.write_buffer += msg;
    Logger::trace("[Send] Buffered message for fd={}, buffer_size={}", fd, conn.write_buffer.size());
    return;
  }

  // 尝试直接发送
  size_t total = 0;
  while (total < msg.size()) {
    ssize_t n = write(fd, msg.data() + total, msg.size() - total);
    if (n > 0) {
      // 成功发送部分数据
      total += static_cast<size_t>(n);
      Logger::trace("[Send] Sent {} bytes to fd={}, progress={}/{}", n, fd, total, msg.size());
    } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // 非阻塞模式下发送缓冲区满，存入写缓冲区
      conn.write_buffer = msg.substr(total);
      if (!conn.write_pending) {
        conn.write_pending = true;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
          Logger::error("[Send] epoll_ctl MOD failed for fd={}: {}, disconnecting", fd, strerror(errno));
          disconnectClient(fd);
          return;
        } else {
          Logger::trace("[Send] Registered EPOLLOUT for fd={}, pending={} bytes", fd, conn.write_buffer.size());
        }
      }
      return;
    } else {
      // 其他错误，断开连接
      Logger::error("[Send] Write error on fd={}: {}, disconnecting", fd, strerror(errno));
      disconnectClient(fd);
      return;
    }
  }
}

// 处理客户端可写事件（刷新写缓冲区）
// 将写缓冲区中的数据发送给客户端，完成后取消EPOLLOUT注册
// 参数 fd: 客户端文件描述符
void EpollServer::handleWrite(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end())
    return;

  Connection &conn = it->second;
  
  // 写缓冲区为空，恢复为只监听EPOLLIN
  if (conn.write_buffer.empty()) {
    conn.write_pending = false;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1)
      Logger::error("handleWrite: epoll_ctl restore failed fd={} err={}", 
                   fd, strerror(errno));
    return;
  }

  // 发送缓冲区中的数据
  size_t total = 0;
  const std::string &data = conn.write_buffer;
  while (total < data.size()) {
    ssize_t n = write(fd, data.data() + total, data.size() - total);
    if (n > 0) {
      // 成功发送部分数据
      total += static_cast<size_t>(n);
    } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // 发送缓冲区满，保留剩余数据
      conn.write_buffer.erase(0, total);
      return;
    } else {
      // 其他错误，断开连接
      Logger::error("Write error on client {}: {}, disconnecting", fd, strerror(errno));
      disconnectClient(fd);
      return;
    }
  }
  
  // 发送完成，清空缓冲区并恢复为只监听EPOLLIN
  conn.write_buffer.clear();
  conn.write_pending = false;
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1)
    Logger::error("handleWrite: epoll_ctl restore failed fd={} err={}", 
                 fd, strerror(errno));
}

// 广播消息给所有客户端
// 参数 msg: 要广播的消息内容
// 参数 exclude_fd: 排除的文件描述符，默认-1表示不排除任何客户端
void EpollServer::broadcast(const std::string &msg, int exclude_fd) {
  // 先收集所有目标fd，避免sendToClient中disconnectClient导致迭代器失效
  std::vector<int> fds;
  fds.reserve(connections_.size());
  for (const auto &[fd, conn] : connections_) {
    (void)conn;
    if (fd != exclude_fd)
      fds.push_back(fd);
  }
  for (int fd : fds) {
    sendToClient(fd, msg);
  }
}

// 根据用户名获取客户端文件描述符
// 参数 username: 用户名
// 返回值: 对应的文件描述符，未找到返回-1
int EpollServer::getUserFd(const std::string &username) {
  auto it = user_map_.find(username);
  if (it != user_map_.end())
    return it->second;
  return -1;
}

// 根据文件描述符获取用户名
// 参数 fd: 文件描述符
// 返回值: 对应的用户名，未找到返回空字符串
std::string EpollServer::getUsername(int fd) {
  auto it = connections_.find(fd);
  if (it != connections_.end())
    return it->second.username;
  return "";
}