// RedisClient - Redis客户端封装类
// 提供分布式用户管理和消息队列功能
// 封装了用户登录/登出、在线状态查询、消息队列操作等原子化操作
// 使用hiredis库进行Redis通信

#include "RedisClient.h"
#include "Logger.h"
#include <vector>

// 匿名命名空间：封装内部辅助函数
namespace {

// 执行带参数的Redis命令
// 参数 ctx: Redis连接上下文
// 参数 args: 命令参数列表
// 返回值: Redis回复对象，调用方负责释放
redisReply *execArgv(redisContext *ctx,
                   const std::vector<std::string> &args) {
  std::vector<const char *> argv;
  std::vector<size_t> argvlen;
  argv.reserve(args.size());
  argvlen.reserve(args.size());
  for (const auto &arg : args) {
    argv.push_back(arg.c_str());
    argvlen.push_back(arg.size());
  }
  return static_cast<redisReply *>(
      redisCommandArgv(ctx, static_cast<int>(args.size()), argv.data(),
                       argvlen.data()));
}

// 检查Redis回复是否有效
// 参数 r: Redis回复对象
// 参数 context: 上下文描述，用于日志输出
// 返回值: 有效返回true，无效返回false
bool checkReply(redisReply *r, const char *context) {
  if (!r) {
    Logger::error("RedisClient: {} command failed or connection lost", context);
    return false;
  }
  if (r->type == REDIS_REPLY_ERROR) {
    Logger::error("RedisClient: {} Redis error: {}", context, r->str);
    freeReplyObject(r);
    return false;
  }
  return true;
}

// 生成用户信息的Redis key
// 参数 username: 用户名
// 返回值: 格式化后的key，格式为 "user:username"
std::string userKey(const std::string &username) {
  return "user:" + username;
}

// 生成消息队列的Redis key
// 参数 target: 目标用户名
// 返回值: 格式化后的key，格式为 "msg_queue:username"
std::string queueKey(const std::string &target) {
  return "msg_queue:" + target;
}

} // namespace

// 构造函数
// 初始化Redis连接上下文为空
RedisClient::RedisClient() : ctx_(nullptr) {}

// 析构函数
// 调用disconnect释放Redis连接资源
RedisClient::~RedisClient() { disconnect(); }

// 连接Redis服务器
// 参数 host: Redis服务器地址，默认127.0.0.1
// 参数 port: Redis端口，默认6379
// 返回值: 成功返回true，失败返回false
bool RedisClient::connect(const std::string &host, int port) {
  ctx_ = redisConnect(host.c_str(), port);
  if (!ctx_ || ctx_->err) {
    if (ctx_) {
      Logger::redisConnectionFailed(host, port, ctx_->errstr);
      redisFree(ctx_);
      ctx_ = nullptr;
    } else {
      Logger::redisConnectionFailed(host, port, "unknown error");
    }
    return false;
  }
  Logger::redisConnected(host, port);
  return true;
}

// 断开Redis连接
// 释放redisContext资源，将ctx_置为空指针
void RedisClient::disconnect() {
  if (ctx_) {
    redisFree(ctx_);
    ctx_ = nullptr;
  }
}

// 检查是否已连接
// 返回值: 已连接且无错误返回true，否则返回false
bool RedisClient::isConnected() const {
  return ctx_ != nullptr && ctx_->err == 0;
}

// 用户登录（原子操作）
// 使用Lua脚本确保原子性，避免多网关并发登录竞态
// 参数 username: 用户名
// 参数 gateway_id: 网关ID
// 参数 fd: 客户端文件描述符
// 返回值: 1=成功, 0=用户已在线(拒绝), -1=错误
int RedisClient::userLogin(const std::string &username, int gateway_id,
                            int fd) {
  // 参数校验
  if (!isConnected()) {
    Logger::error("RedisClient: Not connected for userLogin");
    return -1;
  }
  if (username.empty()) {
    Logger::error("RedisClient: userLogin called with empty username");
    return -1;
  }
  if (fd < 0) {
    Logger::error("RedisClient: userLogin called with invalid fd={}", fd);
    return -1;
  }

  // Lua脚本：原子化检查用户是否已在线，避免多网关并发登录竞态
  // 如果online_users集合中已存在该用户 → 返回0（拒绝登录）
  // 如果不存在 → 执行HSET存储用户信息 + SADD加入在线集合 → 返回1（允许登录）
  const char *lua_script =
      "local member = ARGV[1] "
      "if redis.call('SISMEMBER', 'online_users', member) == 1 then "
      "  return 0 "
      "end "
      "redis.call('HSET', KEYS[1], 'gateway', ARGV[2], 'fd', ARGV[3]) "
      "redis.call('SADD', 'online_users', member) "
      "return 1";

  // 确保C字符串在EVAL调用期间存活（不能直接使用临时std::string的c_str()）
  std::string gw_str = std::to_string(gateway_id);
  std::string fd_str = std::to_string(fd);
  const char *argv_stable[] = {
      username.c_str(),
      gw_str.c_str(),
      fd_str.c_str(),
  };

  std::string key = userKey(username);
  redisReply *r = static_cast<redisReply *>(
      redisCommand(ctx_, "EVAL %s 1 %s %s %s %s",
                   lua_script, key.c_str(),
                   argv_stable[0], argv_stable[1], argv_stable[2]));

  if (!checkReply(r, "EVAL userLogin")) {
    return -1;
  }

  int result = (r->type == REDIS_REPLY_INTEGER && r->integer == 1) ? 1 : 0;
  freeReplyObject(r);

  if (result == 0) {
    Logger::warn("RedisClient: userLogin rejected - user '{}' is already online",
                 username);
  }
  return result;
}

// 用户登出（原子操作）
// 使用MULTI/EXEC事务确保原子性
// 参数 username: 用户名
// 返回值: 成功返回true，失败返回false
bool RedisClient::userLogout(const std::string &username) {
  // 参数校验
  if (!isConnected()) {
    Logger::error("RedisClient: Not connected for userLogout");
    return false;
  }
  if (username.empty()) {
    Logger::error("RedisClient: userLogout called with empty username");
    return false;
  }

  // 开始事务
  redisReply *r = static_cast<redisReply*>(redisCommand(ctx_, "MULTI"));
  if (!checkReply(r, "MULTI"))
    return false;
  freeReplyObject(r);

  // 队列DEL命令：删除用户信息哈希表
  r = execArgv(ctx_, {"DEL", userKey(username)});
  if (!r) {
    Logger::error("RedisClient: Failed to queue DEL command");
    redisReply *discard_reply = static_cast<redisReply*>(redisCommand(ctx_, "DISCARD"));
    if (discard_reply) freeReplyObject(discard_reply);
    return false;
  }
  freeReplyObject(r);

  // 队列SREM命令：从在线用户集合中移除
  r = execArgv(ctx_, {"SREM", "online_users", username});
  if (!r) {
    Logger::error("RedisClient: Failed to queue SREM command");
    redisReply *discard_reply = static_cast<redisReply*>(redisCommand(ctx_, "DISCARD"));
    if (discard_reply) freeReplyObject(discard_reply);
    return false;
  }
  freeReplyObject(r);

  // 执行事务
  r = static_cast<redisReply*>(redisCommand(ctx_, "EXEC"));
  if (!checkReply(r, "EXEC userLogout")) {
    return false;
  }
  freeReplyObject(r);
  return true;
}

// 获取用户所在网关
// 参数 username: 用户名
// 返回值: 网关ID，失败返回-1
int RedisClient::getUserGateway(const std::string &username) {
  if (!isConnected())
    return -1;
  if (username.empty()) {
    Logger::error("RedisClient: getUserGateway called with empty username");
    return -1;
  }

  redisReply *r =
      execArgv(ctx_, {"HGET", userKey(username), "gateway"});
  int g = -1;
  if (r && r->type == REDIS_REPLY_STRING) {
    try {
      g = std::stoi(r->str);
    } catch (const std::invalid_argument &) {
      Logger::warn("RedisClient: Invalid gateway format for user {}: {}", 
                   username, r->str);
    } catch (const std::out_of_range &) {
      Logger::warn("RedisClient: Gateway value out of range for user {}: {}", 
                   username, r->str);
    }
  } else if (r && r->type == REDIS_REPLY_ERROR) {
    Logger::error("RedisClient: HGET gateway error for user {}: {}", 
                  username, r->str);
  }
  if (r)
    freeReplyObject(r);
  return g;
}

// 获取用户的客户端fd
// 参数 username: 用户名
// 返回值: 文件描述符，失败返回-1
int RedisClient::getUserFd(const std::string &username) {
  if (!isConnected())
    return -1;
  if (username.empty()) {
    Logger::error("RedisClient: getUserFd called with empty username");
    return -1;
  }

  redisReply *r = execArgv(ctx_, {"HGET", userKey(username), "fd"});
  int fd = -1;
  if (r && r->type == REDIS_REPLY_STRING) {
    try {
      fd = std::stoi(r->str);
    } catch (const std::invalid_argument &) {
      Logger::warn("RedisClient: Invalid fd format for user {}: {}", 
                   username, r->str);
    } catch (const std::out_of_range &) {
      Logger::warn("RedisClient: FD value out of range for user {}: {}", 
                   username, r->str);
    }
  } else if (r && r->type == REDIS_REPLY_ERROR) {
    Logger::error("RedisClient: HGET fd error for user {}: {}", 
                  username, r->str);
  }
  if (r)
    freeReplyObject(r);
  return fd;
}

// 获取所有在线用户
// 使用管道技术批量获取用户的网关信息，减少网络往返次数
// 返回值: 用户名到网关ID的映射
std::unordered_map<std::string, int> RedisClient::getAllOnlineUsers() {
  std::unordered_map<std::string, int> users;
  if (!isConnected()) {
    Logger::error("RedisClient: Not connected for getAllOnlineUsers");
    return users;
  }

  // 获取在线用户列表
  redisReply *r = execArgv(ctx_, {"SMEMBERS", "online_users"});
  if (!checkReply(r, "SMEMBERS online_users"))
    return users;

  if (r->type != REDIS_REPLY_ARRAY || r->elements == 0) {
    freeReplyObject(r);
    return users;
  }

  // 使用管道批量追加HGET命令
  for (size_t i = 0; i < r->elements; ++i) {
    if (r->element[i]->type == REDIS_REPLY_STRING && r->element[i]->str) {
      std::string key = userKey(r->element[i]->str);
      if (redisAppendCommand(ctx_, "HGET %s gateway", key.c_str()) != REDIS_OK) {
        Logger::error("RedisClient: Failed to append HGET command for user {}", r->element[i]->str);
      } else {
        users[r->element[i]->str] = -1;
      }
    }
  }
  freeReplyObject(r);

  // 批量获取回复
  for (auto &[name, gw] : users) {
    redisReply *hr = nullptr;
    if (redisGetReply(ctx_, reinterpret_cast<void **>(&hr)) != REDIS_OK) {
      Logger::error("RedisClient: Failed to get reply for user {}", name);
      continue;
    }
    if (hr && hr->type == REDIS_REPLY_STRING) {
      try {
        gw = std::stoi(hr->str);
      } catch (const std::invalid_argument &) {
        Logger::warn("RedisClient: Invalid gateway format for user {}", name);
      } catch (const std::out_of_range &) {
        Logger::warn("RedisClient: Gateway value out of range for user {}", name);
      }
    } else if (hr && hr->type == REDIS_REPLY_ERROR) {
      Logger::error("RedisClient: HGET error for user {}: {}", name, hr->str);
    }
    if (hr)
      freeReplyObject(hr);
  }

  return users;
}

// 推送消息到用户队列
// 使用Lua脚本原子化执行RPUSH和SADD，避免消息堆积
// 参数 target: 目标用户名
// 参数 msg: 消息内容
// 返回值: 成功返回true，失败返回false
bool RedisClient::pushMessage(const std::string &target,
                              const std::string &msg) {
  if (!isConnected()) {
    Logger::error("RedisClient: Not connected for pushMessage");
    return false;
  }
  if (target.empty()) {
    Logger::error("RedisClient: pushMessage called with empty target");
    return false;
  }
  if (msg.empty()) {
    Logger::warn("RedisClient: pushMessage called with empty message for target '{}'", target);
  }

  // Lua脚本：原子化RPUSH消息 + SADD标记
  // 避免RPUSH成功但SADD失败导致消息永久堆积在队列中
  const char *lua_script =
      "redis.call('RPUSH', KEYS[1], ARGV[1]) "
      "redis.call('SADD', KEYS[2], ARGV[2]) "
      "return 1";

  std::string queue_key = queueKey(target);
  redisReply *r = static_cast<redisReply *>(
      redisCommand(ctx_, "EVAL %s 2 %s pending_msgs %b %s",
                   lua_script,
                   queue_key.c_str(),
                   msg.data(), msg.size(),
                   target.c_str()));

  if (!checkReply(r, "EVAL pushMessage")) {
    return false;
  }
  freeReplyObject(r);
  return true;
}

// 从用户队列中取出所有消息
// 使用LPOP循环取出队列中所有消息直到队列为空
// 参数 target: 目标用户名
// 返回值: 消息列表
std::vector<std::string> RedisClient::popMessages(const std::string &target) {
  std::vector<std::string> msgs;
  if (!isConnected())
    return msgs;
  if (target.empty()) {
    Logger::error("RedisClient: popMessages called with empty target");
    return msgs;
  }

  while (true) {
    redisReply *r = execArgv(ctx_, {"LPOP", queueKey(target)});
    if (!r) {
      break;
    }
    if (r->type == REDIS_REPLY_ERROR) {
      Logger::error("RedisClient: LPOP error for target '{}': {}", target, r->str);
      freeReplyObject(r);
      break;
    }
    if (r->type != REDIS_REPLY_STRING) {
      freeReplyObject(r);
      break;
    }
    msgs.emplace_back(r->str, r->len);
    freeReplyObject(r);
  }
  return msgs;
}

// 获取并清空有待收消息的用户集合
// 使用Lua脚本原子化操作，避免SMEMBERS和DEL之间的竞态条件
// 返回值: 有待收消息的用户名列表
std::vector<std::string> RedisClient::drainPendingUsers() {
  std::vector<std::string> users;
  if (!isConnected())
    return users;

  // Lua脚本：原子化读取并清空pending_msgs集合
  // 避免SMEMBERS → DEL之间其他网关SADD被误删的竞态
  const char *lua_script =
      "local members = redis.call('SMEMBERS', KEYS[1]) "
      "if #members > 0 then "
      "  redis.call('DEL', KEYS[1]) "
      "end "
      "return members";

  redisReply *r = static_cast<redisReply *>(
      redisCommand(ctx_, "EVAL %s 1 pending_msgs", lua_script));

  if (!r) {
    return users;
  }

  if (r->type == REDIS_REPLY_ERROR) {
    Logger::error("RedisClient: drainPendingUsers error: {}", r->str);
    freeReplyObject(r);
    return users;
  }

  if (r->type == REDIS_REPLY_ARRAY) {
    for (size_t i = 0; i < r->elements; ++i) {
      if (r->element[i]->type == REDIS_REPLY_STRING && r->element[i]->str) {
        users.emplace_back(r->element[i]->str);
      }
    }
  }

  freeReplyObject(r);
  return users;
}