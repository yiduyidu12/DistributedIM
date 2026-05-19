#include "EpollServer.h"
#include "RedisClient.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_EVENTS 1024 // epoll服务器所能处理的最大事件
#define TIMEOUT_SECONDS 300 // 最大静默时间，超过这个时间判定为掉线

// JSON 解析辅助函数
static std::string getJsonString(const std::string &json,
                                 const std::string &key) {
  std::string search = "\"" + key + "\":\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos)
    return "";
  pos += search.length();
  size_t end = json.find("\"", pos);
  if (end == std::string::npos)
    return "";
  return json.substr(pos, end - pos);
}

static int getJsonInt(const std::string &json, const std::string &key) {
  std::string search = "\"" + key + "\":";
  size_t pos = json.find(search);
  if (pos == std::string::npos)
    return 0;
  pos += search.length();
  while (pos < json.length() && json[pos] == ' ')
    pos++;
  size_t end = pos;
  while (end < json.length() && (isdigit(json[end]) || json[end] == '-'))
    end++;
  if (end == pos)
    return 0;
  return std::stoi(json.substr(pos, end - pos));
}

static std::string getJsonType(const std::string &json) {
  return getJsonString(json, "type");
}
// JSON 解析辅助函数结束

EpollServer::EpollServer(int port) : port_(port), server_fd_(-1), epfd_(-1) {
  // 使用时间 + 进程 PID 作为随机种子，确保每个实例的 ID 不同
  srand(time(nullptr) ^ getpid());
  gateway_id_ = rand() % 10000;
}

EpollServer::~EpollServer() {
  if (server_fd_ != -1)
    close(server_fd_);
  if (epfd_ != -1)
    close(epfd_);
}

// 设置非阻塞
static int setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int EpollServer::createServerSocket() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = INADDR_ANY;

  bind(fd, (sockaddr *)&addr, sizeof(addr));
  listen(fd, 10);

  setNonBlocking(fd);

  return fd;
}

void EpollServer::start() {
  // 链接Redis
  if (!redis_.connect()) {
    std::cerr << "Failed to connect to Redis" << std::endl;
    return;
  }
  std::cout << "[INFO] Gateway ID: " << gateway_id_ << std::endl;
  server_fd_ = createServerSocket();

  epfd_ = epoll_create(1);

  // 注册消息处理器
  handler_.registerHandler(
      "login", [this](const std::string &msg, const std::string &username) {
        std::string name = getJsonString(msg, "username");
        if (name.empty())
          return std::string("{\"type\":\"login\",\"status\":\"fail\"}\n");
        return std::string("{\"type\":\"login\",\"status\":\"ok\"}\n");
      });

  handler_.registerHandler("chat", [this](const std::string &msg,
                                          const std::string &username) {
    std::string content = getJsonString(msg, "msg");
    if (content.empty())
      return std::string("");

    std::string sendMsg =
        "{\"type\":\"chat\",\"from\":\"" + username + "\",\"msg\":\"" +
        content + "\",\"timestamp\":" + std::to_string(time(nullptr)) + "}\n";

    // 从 Redis 获取所有在线用户
    auto users = redis_.getAllOnlineUsers();
    for (auto &[name, gateway] : users) {
      if (name == username)
        continue; // 不发给发送者自己
      if (gateway == gateway_id_) {
        // 同一网关，直接发送
        int fd = getUserFd(name);
        if (fd != -1) {
          write(fd, sendMsg.c_str(), sendMsg.size());
        }
      } else {
        // 不同网关，存入 Redis 队列
        redis_.pushMessage(name, sendMsg);
      }
    }

    return std::string("");
  });

  handler_.registerHandler("send", [this](const std::string &msg,
                                          const std::string &username) {
    std::string target = getJsonString(msg, "to");
    std::string content = getJsonString(msg, "msg");
    if (target.empty() || content.empty())
      return std::string("");

    // 从 Redis 查找用户
    int target_gateway = redis_.getUserGateway(target);
    int target_fd = redis_.getUserFd(target);

    if (target_gateway == -1) {
      return std::string("{\"type\":\"error\",\"msg\":\"user not found\"}\n");
    }

    std::string sendMsg = "{\"type\":\"send\",\"from\":\"" + username +
                          "\",\"to\":\"" + target + "\",\"msg\":\"" + content +
                          "\",\"timestamp\":" + std::to_string(time(nullptr)) +
                          "}\n";

    if (target_gateway == gateway_id_) {
      // 同一网关，直接发送
      sendToClient(target_fd, sendMsg);
    } else {
      // 不同网关，存入 Redis 队列
      redis_.pushMessage(target, sendMsg);
    }
    return std::string("");
  });

  handler_.registerHandler("ping", [](const std::string &msg,
                                      const std::string &username) {
    return "{\"type\":\"pong\",\"timestamp\":" + std::to_string(time(nullptr)) +
           "}\n";
  });

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = server_fd_;

  epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd_, &ev);

  std::cout << "Gateway Server started on port " << port_ << std::endl;

  loop();
}

void EpollServer::loop() {
  epoll_event events[MAX_EVENTS];
  while (true) {
    // 设置 100ms 超时，以便定期执行主动拉取
    int nfds = epoll_wait(epfd_, events, MAX_EVENTS, 100);
    time_t now = time(nullptr);

    for (auto it = connections_.begin(); it != connections_.end();) {
      auto &conn = it->second;

      if (now - conn.last_active > TIMEOUT_SECONDS) {
        std::cout << "[TIMEOUT] " << conn.fd_ << std::endl;
        epoll_ctl(epfd_, EPOLL_CTL_DEL, conn.fd_, nullptr);
        close(conn.fd_);
        user_map_.erase(conn.username);

        it = connections_.erase(it);
      } else {
        ++it;
      }
    }

    // 主动拉取 Redis 消息
    for (auto &[fd, conn] : connections_) {
      if (!conn.username.empty()) {
        auto msgs = redis_.popMessages(conn.username);
        for (const auto &msg : msgs) {
          write(fd, msg.c_str(), msg.size());
        }
      }
    }

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;

      if (fd == server_fd_) {
        handleAccept();
      } else {
        handleRead(fd);
      }
    }
  }
}

void EpollServer::handleAccept() {
  int client_fd = accept(server_fd_, nullptr, nullptr);
  setNonBlocking(client_fd);

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = client_fd;

  epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &ev);
  connections_.emplace(client_fd, Connection(client_fd));

  std::cout << "[INFO] New client: " << client_fd << std::endl;
}

void EpollServer::handleRead(int client_fd) {
  char buf[1024] = {0};
  int len = read(client_fd, buf, sizeof(buf));

  auto &conn = connections_.at(client_fd);

  if (len <= 0) {
    std::cout << "[INFO] Client disconnected: " << client_fd << std::endl;
    if (!conn.username.empty()) {
      redis_.userLogout(conn.username);
    }
    close(client_fd);
    connections_.erase(client_fd);
    for (auto it = user_map_.begin(); it != user_map_.end(); ++it) {
      if (it->second == client_fd) {
        user_map_.erase(it);
        break;
      }
    }
    return;
  }

  conn.last_active = time(nullptr);

  std::string msg(buf, len);
  std::string type = getJsonType(msg);

  // 统一处理 JSON 登录
  if (type == "login") {
    std::string name = getJsonString(msg, "username");
    if (!name.empty()) {
      // 存入 Redis
      if (redis_.userLogin(name, gateway_id_, client_fd)) {
        conn.username = name;
        conn.isLogin = true;
        user_map_[name] = client_fd;
        std::string reply = "{\"type\":\"login\",\"status\":\"ok\"}\n";
        write(client_fd, reply.c_str(), reply.size());
        std::cout << "[LOGIN] " << name << " (gateway=" << gateway_id_ << ")"
                  << std::endl;
      } else {
        std::string reply = "{\"type\":\"login\",\"status\":\"fail\"}\n";
        write(client_fd, reply.c_str(), reply.size());
      }
    }
    return;
  }

  // 去掉末尾空白
  while (!msg.empty() &&
         (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) {
    msg.pop_back();
  }

  // 解析命令
  if (msg.find("LOGIN ") == 0) {
    std::string name = msg.substr(6);
    conn.username = name;
    conn.isLogin = true;
    user_map_[name] = client_fd;
    std::string reply = "LOGIN OK\n";
    write(client_fd, reply.c_str(), reply.size());
    std::cout << "[LOGIN] " << name << std::endl;
  } else if (type == "chat") {
    handler_.handle(type, msg, conn.username);
  } else if (type == "send") {
    if (!conn.isLogin) {
      std::string reply = "Please login first\n";
      write(client_fd, reply.c_str(), reply.size());
      return;
    }

    std::string target = getJsonString(msg, "to");
    std::string content = getJsonString(msg, "msg");
    if (target.empty() || content.empty())
      return;

    // 从 Redis 查找用户
    int target_gateway = redis_.getUserGateway(target);
    int target_fd = redis_.getUserFd(target);

    if (target_gateway == -1) {
      std::string reply = "User not found\n";
      write(client_fd, reply.c_str(), reply.size());
      return;
    }

    std::string sendMsg = "{\"type\":\"send\",\"from\":\"" + conn.username +
                          "\",\"to\":\"" + target + "\",\"msg\":\"" + content +
                          "\",\"timestamp\":" + std::to_string(time(nullptr)) +
                          "}\n";

    if (target_gateway == gateway_id_) {
      // 同一网关，检查目标 fd 有效性
      if (target_fd != -1 && target_fd != client_fd) {
        sendToClient(target_fd, sendMsg);
      } else {
        // 无效 fd，降级存入 Redis
        redis_.pushMessage(target, sendMsg);
      }
    } else {
      // 不同网关，存入 Redis 队列
      redis_.pushMessage(target, sendMsg);
    }
  } else if (type == "ping") {
    std::string reply = handler_.handle(type, msg, conn.username);
    if (!reply.empty()) {
      write(client_fd, reply.c_str(), reply.size());
    }
  } else if (type == "who") { // 支持 JSON 格式 WHO
    auto users = redis_.getAllOnlineUsers();
    std::string reply = "{\"type\":\"who\",\"users\":[";
    bool first = true;
    for (auto &[name, gateway] : users) {
      if (!first)
        reply += ",";
      reply += "\"" + name + "\"";
      first = false;
    }
    reply += "]}\n";
    write(client_fd, reply.c_str(), reply.size());
  } else if (msg.find("WHO") == 0) {
    // 从 Redis 获取在线用户
    auto users = redis_.getAllOnlineUsers();
    std::string reply = "{\"type\":\"who\",\"users\":[";
    bool first = true;
    for (auto &[name, gateway] : users) {
      if (!first)
        reply += ",";
      reply += "\"" + name + "\"";
      first = false;
    }
    reply += "]}";
    write(client_fd, reply.c_str(), reply.size());
  } else {
    std::string reply = "Unknown command\n";
    write(client_fd, reply.c_str(), reply.size());
  }

  conn.last_active = std::time(nullptr);
}

void EpollServer::sendToClient(int fd, const std::string &msg) {
  write(fd, msg.c_str(), msg.size());
}

void EpollServer::broadcast(const std::string &msg, int exclude_fd) {
  for (auto &[fd, conn] : connections_) {
    if (fd != exclude_fd) {
      write(fd, msg.c_str(), msg.size());
    }
  }
}

int EpollServer::getUserFd(const std::string &username) {
  auto it = user_map_.find(username);
  if (it != user_map_.end()) {
    return it->second;
  }
  return -1;
}

std::string EpollServer::getUsername(int fd) {
  auto it = connections_.find(fd);
  if (it != connections_.end()) {
    return it->second.username;
  }
  return "";
}