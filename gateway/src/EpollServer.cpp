// EpollServer - 基于Epoll的高性能网关服务器类
// 核心职责：管理TCP/WebSocket连接、处理客户端消息、路由跨网关消息、维护在线用户状态
// 采用Reactor模式，使用Epoll实现高效的IO多路复用
// 支持分布式部署，通过Redis实现跨网关消息路由
// v2.0 新增：WebSocket协议、多设备登录、群组聊天、ACK可靠投递、优先级队列、令牌桶限流

#include "EpollServer.h"
#include "WebSocketCodec.h"
#include "JsonParser.h"
#include "Logger.h"
#include "Config.h"
#include "GatewayRegistry.h"
#include "Metrics.h"
#ifdef E2E_ENABLED
#include "E2EECrypto.h"
#endif
#ifdef AI_SERVICE_ENABLED
#include "AIServiceClient.h"
#include "AuditLogger.h"
#endif

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
#include <algorithm>

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
EpollServer::EpollServer(int port)
    : port_(port), server_fd_(-1), epfd_(-1), group_mgr_(redis_), gateway_registry_(redis_, "gw_" + std::to_string(getpid()) + "_" + std::to_string(port), Config::region().region) {
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

#ifdef AI_SERVICE_ENABLED
  // 关闭审计日志器（等待所有待发送日志完成）
  AuditLogger::instance().shutdown();
#endif
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
// 注册所有消息类型的处理函数（chat/send/ping/who + group_* + ack + login + reply_to）
void EpollServer::registerHandlers() {
  // ============ 群聊消息处理器 ============
  handler_.registerHandler("chat", [this](const std::string &msg,
                                          const std::string &username) {
    Logger::debug("[Handler] chat message received from user='{}'",
                  username.empty() ? "(not logged in)" : username);

    if (username.empty()) {
      Logger::warn("[Chat] Unauthorized chat attempt from non-logged-in user");
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }

    std::string content = getJsonString(msg, "msg");
    if (content.empty()) {
      Logger::warn("[Chat] Empty message content from user='{}'", username);
      return std::string();
    }

    // 生成消息ID（用于ACK追踪）
    std::string msg_id = getJsonString(msg, "msg_id");

    // 构建群聊消息JSON（含消息ID和引用回复支持）
    std::string reply_to = getJsonString(msg, "reply_to");
    nlohmann::json sendJson{{"type", "chat"},
                            {"from", username},
                            {"msg", content},
                            {"timestamp", time(nullptr)}};
    if (!msg_id.empty()) sendJson["msg_id"] = msg_id;
    if (!reply_to.empty()) sendJson["reply_to"] = reply_to;
    std::string sendMsg = sendJson.dump() + "\n";

    // 广播给所有在线用户（用sendToUser支持多设备）
    // 群聊广播消息使用BULK优先级，避免高负载时阻塞关键消息
    auto users = redis_.getAllOnlineUsers();
    Logger::debug("[Chat] Broadcasting to {} online users", users.size());

    for (auto &[name, gateway] : users) {
      if (name == username) continue;

      // ============ E2EE 加密（群聊广播） ============
      // 对每个在线用户单独加密（如果已建立会话）
      std::string msg_to_send = sendMsg;
#ifdef E2E_ENABLED
      SessionState* session = E2EEManager::instance().getSession(name);
      if (session) {
        std::vector<uint8_t> ciphertext;
        if (E2EEManager::instance().encryptMessage(*session, sendMsg, ciphertext)) {
          nlohmann::json e2eeJson{{"type", "e2ee"},
                                  {"from", username},
                                  {"to", name},
                                  {"data", std::string(ciphertext.begin(), ciphertext.end())}};
          msg_to_send = e2eeJson.dump() + "\n";
        }
      }
#endif

      if (gateway == gateway_id_) {
        sendToUser(name, msg_to_send, MessagePriority::BULK);
      } else {
        redis_.pushMessage(name, msg_to_send);
      }
    }
    Logger::info("[Chat] Broadcast completed from user='{}'", username);
#ifdef AI_SERVICE_ENABLED
    // 记录群聊消息审计日志
    AuditLogger::instance().logMessageSent(username, "all", "chat", content.size());
#endif
    return std::string();
  });

  // ============ 私聊消息处理器 ============
  handler_.registerHandler("send", [this](const std::string &msg,
                                          const std::string &username) {
    Logger::debug("[Handler] private message received from user='{}'",
                  username.empty() ? "(not logged in)" : username);

    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }

    std::string target = getJsonString(msg, "to");
    std::string content = getJsonString(msg, "msg");
    std::string msg_id = getJsonString(msg, "msg_id");
    std::string reply_to = getJsonString(msg, "reply_to");

    if (target.empty() || content.empty()) {
      return std::string();
    }

    // 检查目标用户是否存在
    int target_gateway = redis_.getUserGateway(target);
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
    if (!msg_id.empty()) sendJson["msg_id"] = msg_id;
    if (!reply_to.empty()) sendJson["reply_to"] = reply_to;
    std::string sendMsg = sendJson.dump() + "\n";

    // ============ E2EE 加密（发送端） ============
    // 如果与目标用户已建立E2EE会话，则加密消息
#ifdef E2E_ENABLED
    SessionState* e2ee_session = E2EEManager::instance().getSession(target);
    if (e2ee_session) {
      std::vector<uint8_t> ciphertext;
      if (E2EEManager::instance().encryptMessage(*e2ee_session, sendMsg, ciphertext)) {
        nlohmann::json e2eeJson{{"type", "e2ee"},
                                {"from", username},
                                {"to", target},
                                {"data", std::string(ciphertext.begin(), ciphertext.end())}};
        sendMsg = e2eeJson.dump() + "\n";
        Logger::trace("[E2EE] Message encrypted for user '{}'", target);
      }
    }
#endif

    // 判断目标用户是否在本地网关
    // 私聊消息使用NORMAL优先级
    if (target_gateway == gateway_id_) {
      sendToUser(target, sendMsg, MessagePriority::NORMAL);

      // 如果消息有 msg_id，注册ACK追踪
      if (!msg_id.empty()) {
        auto fds = getUserFds(target);
        for (int fd : fds) {
          ack_tracker_.trackMessage(msg_id, target, sendMsg, fd);
        }
      }
    } else {
      redis_.pushMessage(target, sendMsg);
    }
#ifdef AI_SERVICE_ENABLED
    // 记录私聊消息审计日志
    AuditLogger::instance().logMessageSent(username, target, "send", content.size());
#endif
    return std::string();
  });

  // ============ 消息确认处理器（ACK） ============
  handler_.registerHandler("ack", [this](const std::string &msg,
                                         const std::string &username) {
    std::string msg_id = getJsonString(msg, "msg_id");
    if (!msg_id.empty()) {
      ack_tracker_.acknowledgeMessage(msg_id);
      Logger::trace("[ACK] 收到 ack: msg_id={}, from={}", msg_id, username);
    }
    return std::string();
  });

  // ============ 群组操作处理器 ============

  // 创建群组
  handler_.registerHandler("group_create", [this](const std::string &msg,
                                                   const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }
    std::string group_name = getJsonString(msg, "name");
    std::string group_id = group_mgr_.createGroup(group_name, username);
    if (group_id.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "group create failed"}};
      return err.dump() + "\n";
    }
    nlohmann::json reply{{"type", "group_created"}, {"group_id", group_id}, {"name", group_name}};
#ifdef AI_SERVICE_ENABLED
    AuditLogger::instance().logGroupOperation(AuditEventType::GROUP_CREATED, group_id, username);
#endif
    return reply.dump() + "\n";
  });

  // 加入群组
  handler_.registerHandler("group_join", [this](const std::string &msg,
                                                 const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }
    std::string group_id = getJsonString(msg, "group_id");
    if (group_mgr_.joinGroup(group_id, username)) {
      // 通知群内其他成员
      auto members = group_mgr_.getMembers(group_id);
      nlohmann::json notify{{"type", "group_join"}, {"group_id", group_id}, {"username", username}};
      std::string notifyMsg = notify.dump() + "\n";
      for (const auto& member : members) {
        if (member != username) sendToUser(member, notifyMsg);
      }
      nlohmann::json reply{{"type", "group_joined"}, {"group_id", group_id}};
#ifdef AI_SERVICE_ENABLED
      AuditLogger::instance().logGroupOperation(AuditEventType::GROUP_JOINED, group_id, username);
#endif
      return reply.dump() + "\n";
    }
    nlohmann::json err{{"type", "error"}, {"msg", "join failed"}};
    return err.dump() + "\n";
  });

  // 离开群组
  handler_.registerHandler("group_leave", [this](const std::string &msg,
                                                  const std::string &username) {
    std::string group_id = getJsonString(msg, "group_id");
    group_mgr_.leaveGroup(group_id, username);
    // 通知群内其他成员
    auto members = group_mgr_.getMembers(group_id);
    nlohmann::json notify{{"type", "group_leave"}, {"group_id", group_id}, {"username", username}};
    std::string notifyMsg = notify.dump() + "\n";
    for (const auto& member : members) {
      if (member != username) sendToUser(member, notifyMsg);
    }
#ifdef AI_SERVICE_ENABLED
    AuditLogger::instance().logGroupOperation(AuditEventType::GROUP_LEFT, group_id, username);
#endif
    return std::string();
  });

  // 群组消息
  handler_.registerHandler("group_send", [this](const std::string &msg,
                                                 const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }
    std::string group_id = getJsonString(msg, "group_id");
    std::string content = getJsonString(msg, "msg");
    std::string msg_id = getJsonString(msg, "msg_id");

    if (!group_mgr_.isMember(group_id, username)) {
      nlohmann::json err{{"type", "error"}, {"msg", "not a member of this group"}};
      return err.dump() + "\n";
    }

    nlohmann::json sendJson{{"type", "group_send"},
                            {"group_id", group_id},
                            {"from", username},
                            {"msg", content},
                            {"timestamp", time(nullptr)}};
    if (!msg_id.empty()) sendJson["msg_id"] = msg_id;
    std::string sendMsg = sendJson.dump() + "\n";

    auto members = group_mgr_.getMembers(group_id);
    // 群组消息使用BULK优先级
    for (const auto& member : members) {
      if (member != username) {
        // ============ E2EE 加密（群组消息） ============
        std::string msg_to_send = sendMsg;
#ifdef E2E_ENABLED
        SessionState* session = E2EEManager::instance().getSession(member);
        if (session) {
          std::vector<uint8_t> ciphertext;
          if (E2EEManager::instance().encryptMessage(*session, sendMsg, ciphertext)) {
            nlohmann::json e2eeJson{{"type", "e2ee"},
                                    {"from", username},
                                    {"to", member},
                                    {"data", std::string(ciphertext.begin(), ciphertext.end())}};
            msg_to_send = e2eeJson.dump() + "\n";
          }
        }
#endif
        sendToUser(member, msg_to_send, MessagePriority::BULK);
      }
    }
    Logger::info("[Group] 群消息已发送: group={}, from={}", group_id, username);
#ifdef AI_SERVICE_ENABLED
    AuditLogger::instance().logMessageSent(username, group_id, "group_send", content.size());
#endif
    return std::string();
  });

  // ============ 消息反应处理器（Emoji Reaction） ============
  handler_.registerHandler("react", [this](const std::string &msg,
                                           const std::string &username) {
    if (username.empty()) return std::string();
    std::string target_msg_id = getJsonString(msg, "msg_id");
    std::string emoji = getJsonString(msg, "emoji");
    std::string target_user = getJsonString(msg, "to");

    nlohmann::json reaction{{"type", "reaction"},
                            {"from", username},
                            {"msg_id", target_msg_id},
                            {"emoji", emoji},
                            {"timestamp", time(nullptr)}};
    std::string reactionMsg = reaction.dump() + "\n";

    if (!target_user.empty()) {
      // 消息反应使用NORMAL优先级
      sendToUser(target_user, reactionMsg, MessagePriority::NORMAL);
    }
    return std::string();
  });

  // ============ 心跳处理器（ping/pong） ============
  handler_.registerHandler("ping",
                           [this](const std::string &, const std::string &username) {
                             (void)username;
                             nlohmann::json reply{
                                 {"type", "pong"}, {"timestamp", time(nullptr)}};
                             return reply.dump() + "\n";
                           });

  // ============ 在线用户查询处理器 ============
  handler_.registerHandler("who", [this](const std::string &,
                                         const std::string &username) {
    auto users = redis_.getAllOnlineUsers();
    nlohmann::json reply{{"type", "who"}, {"users", nlohmann::json::array()}};
    for (auto &[name, gateway] : users) {
      (void)gateway;
      reply["users"].push_back(name);
    }
    return reply.dump() + "\n";
  });

  // ============ E2EE 密钥交换处理器 ============
#ifdef E2E_ENABLED
  handler_.registerHandler("e2ee_key_exchange", [this](const std::string &msg,
                                                   const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }
    std::string response_json = getJsonString(msg, "key_data");
    if (response_json.empty()) {
      // 请求对方公钥，生成密钥分发请求
      // 密钥交换使用URGENT优先级，确保安全操作优先完成
      std::string target_user = getJsonString(msg, "to");
      std::string request = E2EEManager::instance().createKeyRequest(username);
      sendToUser(target_user, request, MessagePriority::URGENT);
      return std::string();
    }
    // 处理密钥响应并建立端到端加密会话
    std::string from_user = getJsonString(msg, "from");
    if (E2EEManager::instance().handleKeyResponse(from_user, response_json)) {
      Logger::info("[E2EE] 密钥交换成功: {} <-> {}", username, from_user);
      nlohmann::json reply{{"type", "e2ee_ready"}, {"peer", from_user}};
      return reply.dump() + "\n";
    }
    Logger::warn("[E2EE] 密钥交换失败: {} <-> {}", username, from_user);
    nlohmann::json err{{"type", "error"}, {"msg", "key exchange failed"}};
    return err.dump() + "\n";
  });
#endif

  // ============ AI 服务处理器 ============
#ifdef AI_SERVICE_ENABLED
  // AI 聊天请求处理器
  handler_.registerHandler("ai_chat", [this](const std::string &msg,
                                             const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }
    std::string prompt = getJsonString(msg, "prompt");
    std::string context = getJsonString(msg, "context");

    if (prompt.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "prompt is required"}};
      return err.dump() + "\n";
    }

    Logger::info("[AI Chat] User '{}' requested AI chat with prompt: {}",
                 username, prompt.substr(0, 50) + "...");

    ai_client_.chat(prompt, context,
      [this, username](bool success, const std::string& response) {
        nlohmann::json reply{{"type", "ai_response"}, {"success", success}};
        if (success) {
          reply["content"] = response;
          Logger::info("[AI Chat] Response received for user '{}'", username);
        } else {
          reply["error"] = response;
          Logger::warn("[AI Chat] Failed for user '{}': {}", username, response);
        }
        sendToUser(username, reply.dump() + "\n", MessagePriority::URGENT);
      });

    nlohmann::json ack{{"type", "ai_request"}, {"status", "processing"}};
    return ack.dump() + "\n";
  });

  // AI 摘要请求处理器
  handler_.registerHandler("ai_summary", [this](const std::string &msg,
                                                const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }
    std::string messages = getJsonString(msg, "messages");

    if (messages.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "messages is required"}};
      return err.dump() + "\n";
    }

    Logger::info("[AI Summary] User '{}' requested message summary", username);

    ai_client_.summarize(messages,
      [this, username](bool success, const std::string& response) {
        nlohmann::json reply{{"type", "ai_summary"}, {"success", success}};
        if (success) {
          reply["summary"] = response;
          Logger::info("[AI Summary] Response received for user '{}'", username);
        } else {
          reply["error"] = response;
          Logger::warn("[AI Summary] Failed for user '{}': {}", username, response);
        }
        sendToUser(username, reply.dump() + "\n", MessagePriority::URGENT);
      });

    nlohmann::json ack{{"type", "ai_request"}, {"status", "processing"}};
    return ack.dump() + "\n";
  });

  // AI 情感分析请求处理器
  handler_.registerHandler("ai_analyze", [this](const std::string &msg,
                                                const std::string &username) {
    if (username.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
      return err.dump() + "\n";
    }
    std::string text = getJsonString(msg, "text");

    if (text.empty()) {
      nlohmann::json err{{"type", "error"}, {"msg", "text is required"}};
      return err.dump() + "\n";
    }

    Logger::info("[AI Analyze] User '{}' requested sentiment analysis", username);

    ai_client_.analyzeSentiment(text,
      [this, username](bool success, const std::string& response) {
        nlohmann::json reply{{"type", "ai_analyze"}, {"success", success}};
        if (success) {
          reply["sentiment"] = nlohmann::json::parse(response);
          Logger::info("[AI Analyze] Response received for user '{}'", username);
        } else {
          reply["error"] = response;
          Logger::warn("[AI Analyze] Failed for user '{}': {}", username, response);
        }
        sendToUser(username, reply.dump() + "\n", MessagePriority::URGENT);
      });

    nlohmann::json ack{{"type", "ai_request"}, {"status", "processing"}};
    return ack.dump() + "\n";
  });
#endif

}

// 启动服务器
// 依次执行：连接Redis、创建socket、初始化Epoll、注册处理器、进入事件循环
void EpollServer::start() {
  Logger::info("Starting EpollServer on port {}", port_);

  // 步骤1：连接Redis
  if (!redis_.connect(Config::redis().host, Config::redis().port)) {
    Logger::error("[Redis] Failed to connect to {}:{}", Config::redis().host, Config::redis().port);
    return;
  }
  Logger::info("[Redis] Connected successfully to {}:{}", Config::redis().host, Config::redis().port);
  Logger::info("[Gateway] ID: {}", gateway_id_);
  Metrics::instance().incRedisOps("connect");

#ifdef AI_SERVICE_ENABLED
  // 初始化审计日志器（异步批量发送到 Go API 服务）
  AuditLogger::instance().init(Config::ai().audit_api_url);

  // 初始化 AI 服务客户端并集成到 epoll 事件循环
  if (Config::ai().enabled) {
    if (ai_client_.init()) {
      ai_client_.integrateWithEpoll(epfd_);
      Logger::info("[AI] AI service client initialized and integrated with epoll");
    } else {
      Logger::warn("[AI] Failed to initialize AI service client");
    }
  }
#endif

  // 步骤2：创建服务器socket
  server_fd_ = createServerSocket();
  if (server_fd_ == -1) {
    Logger::critical("[Server] Failed to create server socket on port {}, exiting.", port_);
    redis_.disconnect();
    return;
  }

  // 步骤3：创建epoll实例
  epfd_ = epoll_create(1);
  if (epfd_ == -1) {
    Logger::critical("[Epoll] Failed to create epoll instance: {}", strerror(errno));
    close(server_fd_);
    server_fd_ = -1;
    redis_.disconnect();
    return;
  }

  // 步骤4：注册消息处理器
  registerHandlers();
  Logger::debug("[Handler] Registered all message handlers");

  // 步骤4.5：注册到网关注册中心并启动心跳
  gateway_registry_.registerGateway("0.0.0.0", port_);
  gateway_registry_.startHeartbeat(10);
  Logger::info("[GatewayRegistry] 网关注册完成, region={}", Config::region().region);

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
    redis_.disconnect();
    return;
  }

  Logger::info("[Server] Gateway Server started on port {} (fd={})", port_, server_fd_);
  Logger::info("[Server] Max connections: {}, Timeout: {}s",
               Config::server().max_connections, Config::server().timeout_seconds);

  // 步骤6：进入主事件循环
  loop();
}

// 主事件循环
// 循环调用epoll_wait获取事件，依次处理：IO事件、ACK重试、离线消息推送、超时检测
void EpollServer::loop() {
  epoll_event events[MAX_EVENTS];
  while (true) {
    int nfds = epoll_wait(epfd_, events, MAX_EVENTS, 100);
    if (nfds == -1) {
      if (errno == EINTR)
        continue;
      Logger::error("epoll_wait error: {}", strerror(errno));
      continue;
    }

    // 处理所有事件，让活跃连接有机会更新last_active
    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;
      if (fd == server_fd_) {
        handleAccept();
      } else {
        if (events[i].events & EPOLLIN)
          handleRead(fd);
        if (events[i].events & EPOLLOUT)
          handleWrite(fd);

#ifdef AI_SERVICE_ENABLED
        // 将活跃 socket 事件通知 curl_multi（用于 AI HTTP 的非阻塞处理）
        if (Config::ai().enabled) {
          int action = 0;
          if (events[i].events & EPOLLIN)  action |= CURL_CSELECT_IN;
          if (events[i].events & EPOLLOUT) action |= CURL_CSELECT_OUT;
          if (action) ai_client_.performAction(fd, action);
        }
#endif
      }
    }

#ifdef AI_SERVICE_ENABLED
    // 处理已完成的 AI 请求回调
    if (Config::ai().enabled) {
      ai_client_.processCompletedRequests();
    }
#endif

    // 更新 Prometheus Gauge 指标（在线用户数、活跃WS连接数、待确认ACK数）
    Metrics::instance().setOnlineUsers(static_cast<int>(user_map_.size()));
    {
      int ws_count = 0;
      for (const auto& [fd, conn] : connections_) {
        (void)fd;
        if (conn.protocol == Protocol::WEBSOCKET) ws_count++;
      }
      Metrics::instance().setActiveWSConnections(ws_count);
    }
    Metrics::instance().setPendingAcks(static_cast<int>(ack_tracker_.pendingCount()));

    // 处理ACK重试
    checkAckRetries();

    // 处理优先级队列中的消息
    while (!priority_q_.empty()) {
      auto pm = priority_q_.pop();
      if (pm.target_fd != -1) {
        sendToClient(pm.target_fd, pm.payload);
      }
    }

    // 推送Redis中的离线消息
    pushOfflineMessages();

    // 超时检测
    checkTimeouts();

    // 更新网关负载信息（供心跳上报至注册中心）
    gateway_registry_.updateLoad(
        static_cast<int>(connections_.size()),
        GatewayRegistry::calculateLoad(
            static_cast<int>(connections_.size()),
            Config::server().max_connections));
  }
}

// 处理新客户端连接
// 接受客户端连接，设置非阻塞模式，注册到Epoll，初始化Connection对象
void EpollServer::handleAccept() {
  sockaddr_in client_addr{};
  socklen_t addr_len = sizeof(client_addr);

  int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &addr_len);
  if (client_fd == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      Logger::warn("[Accept] Failed to accept new client: {}", strerror(errno));
    return;
  }

  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

  if (setNonBlocking(client_fd) == -1) {
    Logger::warn("[Socket] Failed to set client socket non-blocking for fd={}", client_fd);
    close(client_fd);
    return;
  }

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = client_fd;

  if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
    Logger::warn("[Epoll] Failed to add client socket fd={} to epoll: {}", client_fd, strerror(errno));
    close(client_fd);
    return;
  }

  connections_.emplace(client_fd, Connection(client_fd));
  Metrics::instance().incConnections();
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

  conn.write_buffer.clear();
  conn.write_pending = false;

  // 如果用户已登录，执行登出操作
  if (!username.empty()) {
    // 从 user_map_ 中移除此fd
    auto user_it = user_map_.find(username);
    if (user_it != user_map_.end()) {
      auto& fds = user_it->second;
      fds.erase(std::remove(fds.begin(), fds.end(), client_fd), fds.end());
      // 如果该用户所有设备都下线了，才执行Redis登出
      if (fds.empty()) {
        if (redis_.userLogout(username, gateway_id_, client_fd)) {
          Logger::userLogout(client_fd, username);
        } else {
          Logger::userLogoutFailed(client_fd, username);
        }
        user_map_.erase(user_it);
      } else {
        // 还有其他设备在线，仅从Redis移除当前设备
        redis_.userLogout(username, gateway_id_, client_fd);
        Logger::info("[Logout] 用户 '{}' 设备 fd={} 下线，仍有 {} 台设备在线",
                     username, client_fd, fds.size());
      }
    }
  }

  epoll_ctl(epfd_, EPOLL_CTL_DEL, client_fd, nullptr);
  close(client_fd);
  connections_.erase(it);
  Metrics::instance().decConnections();

#ifdef AI_SERVICE_ENABLED
  // 记录连接断开审计日志
  if (!username.empty()) {
    AuditEntry entry;
    entry.event_type = AuditEventType::CONNECTION_DROP;
    entry.username = username;
    entry.timestamp = time(nullptr);
    AuditLogger::instance().log(entry);
  }
#endif

  Logger::clientDisconnected(client_fd, username, connections_.size());
}

// ============ WebSocket 升级处理 ============

// 检测 HTTP Upgrade 请求并完成 WebSocket 握手
// 参数 client_fd: 客户端文件描述符
// 参数 conn: 连接对象
// 参数 data: 客户端发送的第一段数据（可能是HTTP Upgrade请求）
// 返回值: 成功升级返回true
bool EpollServer::tryWebSocketUpgrade(int client_fd, Connection& conn,
                                       const std::string& data) {
  if (!WebSocketCodec::isUpgradeRequest(data)) return false;

  Logger::info("[WS] 检测到 WebSocket Upgrade 请求: fd={}", client_fd);

  // 生成握手响应
  std::string response = WebSocketCodec::generateHandshakeResponse(data);

  // 发送 HTTP 101 响应
  size_t total = 0;
  while (total < response.size()) {
    ssize_t n = write(client_fd, response.data() + total, response.size() - total);
    if (n <= 0) {
      Logger::error("[WS] 握手响应发送失败: fd={}", client_fd);
      return false;
    }
    total += static_cast<size_t>(n);
  }

  // 切换连接协议
  conn.protocol = Protocol::WEBSOCKET;
  conn.ws.reset();

  Logger::info("[WS] 握手完成，协议切换为 WebSocket: fd={}", client_fd);
  return true;
}

// ============ 消息读取处理 ============

// 处理客户端可读事件
// 根据协议类型分发到 TCP 或 WebSocket 读取函数
void EpollServer::handleRead(int client_fd) {
  auto it = connections_.find(client_fd);
  if (it == connections_.end()) return;

  Connection &conn = it->second;

  if (conn.protocol == Protocol::WEBSOCKET) {
    handleWsRead(client_fd, conn);
  } else {
    handleTcpRead(client_fd, conn);
  }
}

// TCP 协议消息读取（按换行符分割）
void EpollServer::handleTcpRead(int client_fd, Connection& conn) {
  char buf[1024];

  while (true) {
    int len = read(client_fd, buf, sizeof(buf));

    if (len == 0) {
      Logger::info("[Read] Client closed connection: fd={}", client_fd);
      disconnectClient(client_fd);
      return;
    }

    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      Logger::error("[Read] Error reading from fd={}: {}", client_fd, strerror(errno));
      disconnectClient(client_fd);
      return;
    }

    conn.last_active = time(nullptr);
    conn.read_buffer.append(buf, len);

    // 记录接收字节数到 Prometheus 指标
    Metrics::instance().addBytesReceived(static_cast<uint64_t>(len));

    // 检测 HTTP GET /metrics 请求（Prometheus 抓取）
    if (conn.read_buffer.find("GET /metrics") != std::string::npos) {
      size_t header_end = conn.read_buffer.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        std::string response = buildHttpMetricsResponse();
        // 尽力发送 HTTP 响应
        size_t total = 0;
        while (total < response.size()) {
          ssize_t n = write(client_fd, response.data() + total, response.size() - total);
          if (n <= 0) break;
          total += static_cast<size_t>(n);
        }
        conn.read_buffer.clear();
        disconnectClient(client_fd);
        return;
      }
    }

    // 检测 WebSocket Upgrade 请求（第一个数据包可能是 HTTP Upgrade）
    if (conn.protocol == Protocol::UNKNOWN &&
        conn.read_buffer.find("Upgrade: websocket") != std::string::npos) {
      // 检查是否包含完整HTTP头（以 \r\n\r\n 结束）
      size_t header_end = conn.read_buffer.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        std::string http_request = conn.read_buffer.substr(0, header_end + 4);
        conn.read_buffer.erase(0, header_end + 4);
        if (tryWebSocketUpgrade(client_fd, conn, http_request)) {
          // 升级成功，剩余数据作为 WebSocket 帧处理
          if (!conn.read_buffer.empty()) {
            // 将剩余TCP数据移到WS原始缓冲区
            conn.ws.raw_buffer.insert(conn.ws.raw_buffer.end(),
                                       conn.read_buffer.begin(),
                                       conn.read_buffer.end());
            conn.read_buffer.clear();
            handleWsRead(client_fd, conn);
          }
          return;
        }
      }
    }

    // 按换行符分割消息
    size_t pos = 0;
    while ((pos = conn.read_buffer.find('\n')) != std::string::npos) {
      std::string line = conn.read_buffer.substr(0, pos);
      conn.read_buffer.erase(0, pos + 1);
      handleMessage(client_fd, conn, line);
    }

    // 缓冲区溢出检测
    if (conn.read_buffer.size() > 65536) {
      Logger::error("[Read] Buffer overflow for fd={}, size={}", client_fd, conn.read_buffer.size());
      disconnectClient(client_fd);
      return;
    }
  }
}

// WebSocket 协议消息读取（帧解析状态机）
void EpollServer::handleWsRead(int client_fd, Connection& conn) {
  char buf[1024];

  while (true) {
    int len = read(client_fd, buf, sizeof(buf));

    if (len == 0) {
      Logger::info("[WS] Client closed connection: fd={}", client_fd);
      disconnectClient(client_fd);
      return;
    }

    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      Logger::error("[WS] Error reading from fd={}: {}", client_fd, strerror(errno));
      disconnectClient(client_fd);
      return;
    }

    conn.last_active = time(nullptr);

    // 记录接收字节数到 Prometheus 指标
    Metrics::instance().addBytesReceived(static_cast<uint64_t>(len));

    // 追加原始字节到 WS 解析缓冲区
    conn.ws.raw_buffer.insert(conn.ws.raw_buffer.end(), buf, buf + len);

    // 循环解析帧
    while (true) {
      WsFrame frame;
      bool has_frame = WebSocketCodec::tryParseFrame(
          conn.ws.raw_buffer, frame, conn.ws.parse_state);

      if (!has_frame) {
        // 检查是否为载荷过大错误
        if (conn.ws.parse_state.stage == WsParseStage::PAYLOAD &&
            conn.ws.parse_state.payload_length > WebSocketCodec::maxPayloadSize()) {
          Logger::warn("[WS] 消息过大: fd={}, size={}", client_fd,
                       conn.ws.parse_state.payload_length);
          std::string close_frame = WebSocketCodec::encodeCloseString(
              WsCloseCode::MESSAGE_TOO_BIG);
          sendToClient(client_fd, close_frame);
          disconnectClient(client_fd);
          return;
        }
        break;  // 等待更多数据
      }

      // 处理控制帧（Ping/Pong/Close）
      if (frame.is_control) {
        handleWsControlFrame(client_fd, conn, frame);
        continue;  // 控制帧不进入消息处理
      }

      // 处理数据帧（Text/Binary/Continuation）
      if (WebSocketCodec::needsReassembly(frame, conn.ws.in_fragmented)) {
        if (!conn.ws.in_fragmented) {
          conn.ws.in_fragmented = true;
          conn.ws.fragment_opcode = frame.opcode;
        }
        WebSocketCodec::appendToReassembly(conn.ws.reassembly_buf, frame);

        if (frame.fin) {
          std::string message = WebSocketCodec::finalizeReassembly(
              conn.ws.reassembly_buf, conn.ws.fragment_opcode);
          conn.ws.in_fragmented = false;
          handleMessage(client_fd, conn, message);
        }
      } else {
        // 单帧完整消息
        std::string message(frame.payload.begin(), frame.payload.end());
        handleMessage(client_fd, conn, message);
      }
    }
  }
}

// 处理 WebSocket 控制帧
// 参数 client_fd: 客户端文件描述符
// 参数 conn: 连接对象
// 参数 frame: 已解析的控制帧
void EpollServer::handleWsControlFrame(int client_fd, Connection& conn,
                                        const WsFrame& frame) {
  switch (static_cast<WsOpcode>(frame.opcode)) {
    case WsOpcode::PING: {
      Logger::trace("[WS] 收到 Ping: fd={}", client_fd);
      conn.last_active = time(nullptr);
      auto pong_data = WebSocketCodec::encodePong(frame.payload);
      conn.write_buffer.append(reinterpret_cast<const char*>(pong_data.data()), pong_data.size());
      if (!conn.write_pending) {
        conn.write_pending = true;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = client_fd;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, client_fd, &ev);
      }
      break;
    }
    case WsOpcode::PONG:
      Logger::trace("[WS] 收到 Pong: fd={}", client_fd);
      conn.last_active = time(nullptr);
      break;
    case WsOpcode::CLOSE: {
      Logger::info("[WS] 收到 Close 帧: fd={}", client_fd);
      std::string close_resp = WebSocketCodec::encodeCloseString(WsCloseCode::NORMAL);
      sendToClient(client_fd, close_resp);
      disconnectClient(client_fd);
      break;
    }
    default:
      break;
  }
}

// ============ 消息处理 ============

// 处理客户端消息
// 根据消息类型分发到相应的处理器，先经过限流检查
void EpollServer::handleMessage(int client_fd, Connection &conn,
                                const std::string &rawMsg) {
  std::string msg = trimTrailingWhitespace(rawMsg);
  if (msg.empty()) {
    Logger::trace("[Message] Empty message received from fd={}", client_fd);
    return;
  }

  // ============ E2EE 解密（接收端） ============
  // 如果消息是加密的（type == "e2ee"），先解密再处理
#ifdef E2E_ENABLED
  if (getJsonType(msg) == "e2ee") {
    std::string encrypted_data = getJsonString(msg, "data");
    std::string from_user = getJsonString(msg, "from");
    if (!encrypted_data.empty() && !from_user.empty()) {
      SessionState* session = E2EEManager::instance().getSession(from_user);
      if (session) {
        std::vector<uint8_t> ciphertext(encrypted_data.begin(), encrypted_data.end());
        std::string plaintext;
        if (E2EEManager::instance().decryptMessage(*session, ciphertext, plaintext)) {
          msg = plaintext;
          Logger::trace("[E2EE] Message decrypted from user '{}'", from_user);
        } else {
          Logger::warn("[E2EE] Failed to decrypt message from user '{}'", from_user);
          return;
        }
      } else {
        Logger::warn("[E2EE] No session found for user '{}', dropping encrypted message", from_user);
        return;
      }
    }
  }
#endif

  std::string type = getJsonType(msg);
  // 记录消息指标（按类型分类）
  if (!type.empty()) Metrics::instance().incMessages(type);
  Logger::messageReceived(client_fd, conn.username, msg.size(), type);

  // 处理纯文本LOGIN命令
  if (msg.rfind("LOGIN ", 0) == 0) {
    std::string username = trimTrailingWhitespace(msg.substr(6));
    if (Config::rateLimit().enabled && !conn.rate_limiter.tryConsume()) {
      Metrics::instance().incRateLimitHits();
#ifdef AI_SERVICE_ENABLED
      AuditLogger::instance().logRateLimitHit(username, conn.rate_limiter.retryAfterSeconds());
#endif
      nlohmann::json err{{"type", "error"}, {"msg", "rate limit exceeded"},
                         {"retry_after", conn.rate_limiter.retryAfterSeconds()}};
      priority_q_.push(err.dump() + "\n", client_fd, MessagePriority::URGENT);
      return;
    }
    performLogin(client_fd, conn, username);
    return;
  }

  // 处理纯文本WHO命令
  if (msg == "WHO" || msg.rfind("WHO ", 0) == 0) {
    std::string reply = handler_.handle("who", msg, conn.username);
    if (!reply.empty())
      sendToClient(client_fd, reply);
    return;
  }

  // 处理JSON格式登录请求
  if (type == "login") {
    if (Config::rateLimit().enabled && !conn.rate_limiter.tryConsume()) {
      Metrics::instance().incRateLimitHits();
      std::string uname = getJsonString(msg, "username");
#ifdef AI_SERVICE_ENABLED
      AuditLogger::instance().logRateLimitHit(uname, conn.rate_limiter.retryAfterSeconds());
#endif
      nlohmann::json err{{"type", "error"}, {"msg", "rate limit exceeded"},
                         {"retry_after", conn.rate_limiter.retryAfterSeconds()}};
      priority_q_.push(err.dump() + "\n", client_fd, MessagePriority::URGENT);
      return;
    }
    std::string username = getJsonString(msg, "username");
    std::string device_id = getJsonString(msg, "device_id");
    if (!device_id.empty()) conn.device_id = device_id;
    performLogin(client_fd, conn, username);
    return;
  }

  // 心跳处理
  if (type == "ping") {
    Logger::heartbeatReceived(client_fd, conn.username);
    std::string reply = handler_.handle("ping", msg, conn.username);
    if (!reply.empty()) {
      sendToClient(client_fd, reply);
      Logger::heartbeatSent(client_fd);
    }
    return;
  }

  // 检查用户是否已登录
  if (!conn.isLogin) {
    Logger::unauthorizedAccess(client_fd, type);
    nlohmann::json err{{"type", "error"}, {"msg", "please login first"}};
    sendToClient(client_fd, err.dump() + "\n");
    return;
  }

  // 应用层限流
  if (Config::rateLimit().enabled && !conn.rate_limiter.tryConsume()) {
    Metrics::instance().incRateLimitHits();
#ifdef AI_SERVICE_ENABLED
    AuditLogger::instance().logRateLimitHit(conn.username, conn.rate_limiter.retryAfterSeconds());
#endif
    nlohmann::json err{{"type", "error"}, {"msg", "rate limit exceeded"},
                       {"retry_after", conn.rate_limiter.retryAfterSeconds()}};
    priority_q_.push(err.dump() + "\n", client_fd, MessagePriority::URGENT);
    Logger::warn("[RateLimit] 用户 '{}' 触发限流 (fd={})", conn.username, client_fd);
    return;
  }

  // 未知消息类型
  if (type.empty()) {
    Logger::unknownCommand(client_fd, msg);
    sendToClient(client_fd, "Unknown command\n");
    return;
  }

  // 分发到消息处理器
  std::string reply = handler_.handle(type, msg, conn.username);
  if (!reply.empty())
    sendToClient(client_fd, reply);
}

// ============ 用户登录 ============

// 执行用户登录操作
// 支持多设备登录：user_map_ 存储 username → vector<fd> 映射
bool EpollServer::performLogin(int client_fd, Connection &conn,
                               const std::string &name) {
  Logger::debug("[Login] Login attempt: fd={}, username='{}'", client_fd, name);

  if (name.empty()) {
    Logger::userLoginFailed(client_fd, name, "empty username");
    nlohmann::json reply{{"type", "login"}, {"status", "fail"}, {"msg", "username cannot be empty"}};
    sendToClient(client_fd, reply.dump() + "\n");
    return false;
  }

  // 执行Redis用户登录
  int login_result = redis_.userLogin(name, gateway_id_, client_fd);
  if (login_result == 1) {
    // 登录成功，更新连接状态
    conn.username = name;
    conn.isLogin = true;
    conn.last_active = time(nullptr);

    // 多设备支持：追加到用户fd列表
    user_map_[name].push_back(client_fd);

    // 发送成功响应
    nlohmann::json reply{{"type", "login"}, {"status", "ok"}, {"gateway_id", gateway_id_}};
    sendToClient(client_fd, reply.dump() + "\n");

#ifdef AI_SERVICE_ENABLED
    // 记录登录成功审计日志
    AuditLogger::instance().logLoginAttempt(name, true, "N/A");
#endif

    Logger::userLogin(client_fd, name, gateway_id_, user_map_.size());
    return true;
  }

  // 登录失败
  if (login_result == 0) {
    // 用户已在其他设备在线（多设备支持：允许同网关再次登录）
    bool is_multi_device = true;
    auto it = user_map_.find(name);
    if (it != user_map_.end()) {
      conn.username = name;
      conn.isLogin = true;
      conn.last_active = time(nullptr);
      it->second.push_back(client_fd);

      nlohmann::json reply{{"type", "login"}, {"status", "ok"},
                           {"gateway_id", gateway_id_}, {"multi_device", true}};
      sendToClient(client_fd, reply.dump() + "\n");

#ifdef AI_SERVICE_ENABLED
      // 记录多设备登录成功审计日志
      AuditLogger::instance().logLoginAttempt(name, true, "N/A");
#endif

      Logger::userLogin(client_fd, name, gateway_id_, user_map_.size());
      Logger::info("[Login] 多设备登录: user='{}', 新设备fd={}", name, client_fd);
      return true;
    }

#ifdef AI_SERVICE_ENABLED
    // 记录登录失败审计日志（用户已在其他网关在线）
    AuditLogger::instance().logLoginAttempt(name, false, "N/A", "user already online on another gateway");
#endif

    Logger::userLoginFailed(client_fd, name, "user already online on another gateway");
    nlohmann::json reply{{"type", "login"}, {"status", "fail"}, {"msg", "user already logged in on another gateway"}};
    sendToClient(client_fd, reply.dump() + "\n");
  } else {
#ifdef AI_SERVICE_ENABLED
    // 记录登录失败审计日志（Redis 操作失败）
    AuditLogger::instance().logLoginAttempt(name, false, "N/A", "Redis login failed");
#endif

    Logger::userLoginFailed(client_fd, name, "Redis login failed");
    nlohmann::json reply{{"type", "login"}, {"status", "fail"}, {"msg", "login failed"}};
    sendToClient(client_fd, reply.dump() + "\n");
  }
  return false;
}

// ============ 消息发送 ============

// 向客户端发送消息
// 自动根据协议类型选择发送方式（TCP原样 / WebSocket帧包装）
void EpollServer::sendToClient(int fd, const std::string &msg) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) return;

  Connection &conn = it->second;
  std::string data_to_send;

  // 根据协议类型包装消息
  if (conn.protocol == Protocol::WEBSOCKET) {
    data_to_send = WebSocketCodec::encodeTextString(msg);
  } else {
    data_to_send = msg;
  }

  // 记录发送字节数到 Prometheus 指标
  Metrics::instance().addBytesSent(data_to_send.size());

  // 如果写缓冲区不为空，追加到缓冲区
  if (!conn.write_buffer.empty()) {
    conn.write_buffer += data_to_send;
    return;
  }

  // 尝试直接发送
  size_t total = 0;
  while (total < data_to_send.size()) {
    ssize_t n = write(fd, data_to_send.data() + total, data_to_send.size() - total);
    if (n > 0) {
      total += static_cast<size_t>(n);
    } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      conn.write_buffer = data_to_send.substr(total);
      if (!conn.write_pending) {
        conn.write_pending = true;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
          Logger::error("[Send] epoll_ctl MOD failed for fd={}: {}", fd, strerror(errno));
          disconnectClient(fd);
        }
      }
      return;
    } else {
      Logger::error("[Send] Write error on fd={}: {}", fd, strerror(errno));
      disconnectClient(fd);
      return;
    }
  }
}

// 向用户的所有设备发送消息（多设备广播）
// 通过优先级队列发送，确保消息按优先级顺序投递
// 参数 username: 目标用户名
// 参数 msg: 消息内容
// 参数 priority: 消息优先级（默认NORMAL）
void EpollServer::sendToUser(const std::string& username, const std::string& msg,
                             MessagePriority priority) {
  auto it = user_map_.find(username);
  if (it == user_map_.end()) return;

  for (int fd : it->second) {
    priority_q_.push(msg, fd, priority);
  }
}

// 处理客户端可写事件（刷新写缓冲区）
void EpollServer::handleWrite(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) return;

  Connection &conn = it->second;

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

  size_t total = 0;
  const std::string &data = conn.write_buffer;
  while (total < data.size()) {
    ssize_t n = write(fd, data.data() + total, data.size() - total);
    if (n > 0) {
      total += static_cast<size_t>(n);
    } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      conn.write_buffer.erase(0, total);
      return;
    } else {
      Logger::error("Write error on client {}: {}, disconnecting", fd, strerror(errno));
      disconnectClient(fd);
      return;
    }
  }

  conn.write_buffer.clear();
  conn.write_pending = false;
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1)
    Logger::error("handleWrite: epoll_ctl restore failed fd={} err={}",
                 fd, strerror(errno));
}

// ============ ACK 重试 ============

// ACK 重试检查
// 在事件循环中定期调用，检查需要重试的消息
void EpollServer::checkAckRetries() {
  auto retries = ack_tracker_.getPendingRetries();
  for (const auto& entry : retries) {
    Logger::trace("[ACK] 重试消息: msg_id={}, 重试#{}, target_fd={}",
                  entry.msg_id, entry.retry_count + 1, entry.target_fd);
    sendToClient(entry.target_fd, entry.payload);
    ack_tracker_.markRetryDone(entry.msg_id);
  }

  // 处理死信队列
  auto dead_letters = ack_tracker_.drainDeadLetters();
  for (const auto& entry : dead_letters) {
    // 将死信存入 Redis 死信队列
    std::string dead_key = "dead_letter:" + entry.target_user;
    redis_.pushMessage(dead_key, entry.payload);
    Logger::warn("[ACK] 消息进入死信队列: msg_id={}, user={}",
                 entry.msg_id, entry.target_user);
  }
}

// ============ 离线消息 ============

// 离线消息推送
// 从Redis拉取并投递给本地在线用户
void EpollServer::pushOfflineMessages() {
  auto pending = redis_.drainPendingUsers();
  for (const auto &name : pending) {
    auto msgs = redis_.popMessages(name);
    for (const auto &msg : msgs)
      sendToUser(name, msg);
  }
}

// ============ 超时检测 ============

// 超时检测
// 检查所有连接，断开超过timeout_seconds未活动的连接
void EpollServer::checkTimeouts() {
  time_t now = time(nullptr);
  std::vector<int> timed_out_fds;
  for (const auto& [fd, conn] : connections_) {
    if (now - conn.last_active > Config::server().timeout_seconds) {
      Logger::connectionTimeout(fd, conn.username);
      timed_out_fds.push_back(fd);
    }
  }
  for (int fd : timed_out_fds) {
    disconnectClient(fd);
  }
}

// ============ 辅助方法 ============

// 广播消息给所有客户端
void EpollServer::broadcast(const std::string &msg, int exclude_fd) {
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

// 根据用户名获取所有设备fd
std::vector<int> EpollServer::getUserFds(const std::string &username) {
  auto it = user_map_.find(username);
  if (it != user_map_.end())
    return it->second;
  return {};
}

// 根据用户名获取主设备fd（第一个在线设备）
int EpollServer::getUserFd(const std::string &username) {
  auto fds = getUserFds(username);
  return fds.empty() ? -1 : fds[0];
}

// 根据文件描述符获取用户名
std::string EpollServer::getUsername(int fd) {
  auto it = connections_.find(fd);
  if (it != connections_.end())
    return it->second.username;
  return "";
}

// 构建 HTTP /metrics 响应（供 Prometheus 抓取）
// 返回标准的 HTTP 1.1 响应，Content-Type 为 Prometheus 文本格式
std::string EpollServer::buildHttpMetricsResponse() const {
  std::string metrics = Metrics::instance().exportPrometheusText();
  std::string response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Length: " + std::to_string(metrics.size()) + "\r\n"
      "Connection: close\r\n"
      "\r\n" + metrics;
  return response;
}
