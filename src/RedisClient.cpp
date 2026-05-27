// RedisClient - Redis客户端封装类
// 主要功能：用户登录/登出（原子操作）、获取用户网关信息、在线用户管理、跨网关消息队列

#include "RedisClient.h"

#include <iostream>
#include <vector>

namespace {

// 执行Redis命令（带参数数组）
// ctx: Redis上下文
// args: 命令参数列表
// 返回值: 返回Redis回复对象，调用者负责释放
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

// 检查Redis回复是否正常
// r: Redis回复对象
// context: 上下文描述（用于错误日志）
// 返回值: 正常返回true，错误返回false
bool checkReply(redisReply *r, const char *context) {
  if (!r) {
    std::cerr << "RedisClient: " << context
              << " command failed or connection lost" << std::endl;
    return false;
  }
  if (r->type == REDIS_REPLY_ERROR) {
    std::cerr << "RedisClient: " << context << " Redis error: " << r->str
              << std::endl;
    freeReplyObject(r);
    return false;
  }
  return true;
}

// 生成用户信息的Redis key
// username: 用户名
// 返回值: key字符串，格式为 "user:<username>"
std::string userKey(const std::string &username) {
  return "user:" + username;
}

// 生成消息队列的Redis key
// target: 目标用户名
// 返回值: key字符串，格式为 "msg_queue:<target>"
std::string queueKey(const std::string &target) {
  return "msg_queue:" + target;
}

} // namespace

// RedisClient构造函数
RedisClient::RedisClient() : ctx_(nullptr) {}

// RedisClient析构函数
// 自动断开Redis连接
RedisClient::~RedisClient() { disconnect(); }

// 连接Redis服务器
// host: Redis服务器地址（默认127.0.0.1）
// port: Redis端口（默认6379）
// 返回值: 成功返回true，失败返回false
bool RedisClient::connect(const std::string &host, int port) {
  ctx_ = redisConnect(host.c_str(), port);
  if (!ctx_ || ctx_->err) {
    if (ctx_)
      std::cerr << "Redis error: " << ctx_->errstr << std::endl;
    else
      std::cerr << "Redis connection failed" << std::endl;
    return false;
  }
  std::cout << "[Redis] Connected to Redis server" << std::endl;
  return true;
}

// 断开Redis连接
void RedisClient::disconnect() {
  if (ctx_) {
    redisFree(ctx_);
    ctx_ = nullptr;
  }
}

// 用户登录（原子操作）
// 使用MULTI/EXEC事务确保HSET和SADD操作的原子性
// username: 用户名
// gateway_id: 网关ID
// fd: 客户端文件描述符
// 返回值: 成功返回true
bool RedisClient::userLogin(const std::string &username, int gateway_id,
                            int fd) {
  if (!ctx_) {
    std::cerr << "RedisClient: Not connected for userLogin" << std::endl;
    return false;
  }

  // 开始事务
  redisReply *r = redisCommand(ctx_, "MULTI");
  if (!checkReply(r, "MULTI"))
    return false;
  freeReplyObject(r);

  // 入队HSET命令：存储用户网关和fd信息
  r = execArgv(ctx_, {"HSET",
                      userKey(username),
                      "gateway",
                      std::to_string(gateway_id),
                      "fd",
                      std::to_string(fd)});
  if (!r) {
    std::cerr << "RedisClient: Failed to queue HSET command" << std::endl;
    redisCommand(ctx_, "DISCARD");
    return false;
  }
  freeReplyObject(r);

  // 入队SADD命令：将用户加入在线用户集合
  r = execArgv(ctx_, {"SADD", "online_users", username});
  if (!r) {
    std::cerr << "RedisClient: Failed to queue SADD command" << std::endl;
    redisCommand(ctx_, "DISCARD");
    return false;
  }
  freeReplyObject(r);

  // 执行事务
  r = redisCommand(ctx_, "EXEC");
  if (!checkReply(r, "EXEC userLogin")) {
    return false;
  }
  freeReplyObject(r);
  return true;
}

// 用户登出（原子操作）
// 使用MULTI/EXEC事务确保DEL和SREM操作的原子性
// username: 用户名
// 返回值: 成功返回true
bool RedisClient::userLogout(const std::string &username) {
  if (!ctx_) {
    std::cerr << "RedisClient: Not connected for userLogout" << std::endl;
    return false;
  }

  // 开始事务
  redisReply *r = redisCommand(ctx_, "MULTI");
  if (!checkReply(r, "MULTI"))
    return false;
  freeReplyObject(r);

  // 入队DEL命令：删除用户信息
  r = execArgv(ctx_, {"DEL", userKey(username)});
  if (!r) {
    std::cerr << "RedisClient: Failed to queue DEL command" << std::endl;
    redisCommand(ctx_, "DISCARD");
    return false;
  }
  freeReplyObject(r);

  // 入队SREM命令：将用户从在线用户集合中移除
  r = execArgv(ctx_, {"SREM", "online_users", username});
  if (!r) {
    std::cerr << "RedisClient: Failed to queue SREM command" << std::endl;
    redisCommand(ctx_, "DISCARD");
    return false;
  }
  freeReplyObject(r);

  // 执行事务
  r = redisCommand(ctx_, "EXEC");
  if (!checkReply(r, "EXEC userLogout")) {
    return false;
  }
  freeReplyObject(r);
  return true;
}

// 获取用户所在网关
// username: 用户名
// 返回值: 成功返回网关ID，失败返回-1
int RedisClient::getUserGateway(const std::string &username) {
  if (!ctx_)
    return -1;

  redisReply *r =
      execArgv(ctx_, {"HGET", userKey(username), "gateway"});
  int g = -1;
  if (r && r->type == REDIS_REPLY_STRING) {
    try {
      g = std::stoi(r->str);
    } catch (const std::invalid_argument &) {
      std::cerr << "RedisClient: Invalid gateway format for user " << username
                << ": " << r->str << std::endl;
    } catch (const std::out_of_range &) {
      std::cerr << "RedisClient: Gateway value out of range for user "
                << username << ": " << r->str << std::endl;
    }
  } else if (r && r->type == REDIS_REPLY_ERROR) {
    std::cerr << "RedisClient: HGET gateway error for user " << username << ": "
              << r->str << std::endl;
  }
  if (r)
    freeReplyObject(r);
  return g;
}

// 获取用户的客户端fd
// username: 用户名
// 返回值: 成功返回fd，失败返回-1
int RedisClient::getUserFd(const std::string &username) {
  if (!ctx_)
    return -1;

  redisReply *r = execArgv(ctx_, {"HGET", userKey(username), "fd"});
  int fd = -1;
  if (r && r->type == REDIS_REPLY_STRING) {
    try {
      fd = std::stoi(r->str);
    } catch (const std::invalid_argument &) {
      std::cerr << "RedisClient: Invalid fd format for user " << username
                << ": " << r->str << std::endl;
    } catch (const std::out_of_range &) {
      std::cerr << "RedisClient: FD value out of range for user " << username
                << ": " << r->str << std::endl;
    }
  } else if (r && r->type == REDIS_REPLY_ERROR) {
    std::cerr << "RedisClient: HGET fd error for user " << username << ": "
              << r->str << std::endl;
  }
  if (r)
    freeReplyObject(r);
  return fd;
}

// 获取所有在线用户
// 返回值: 用户名到网关ID的映射
std::unordered_map<std::string, int> RedisClient::getAllOnlineUsers() {
  std::unordered_map<std::string, int> users;
  if (!ctx_) {
    std::cerr << "RedisClient: Not connected for getAllOnlineUsers"
              << std::endl;
    return users;
  }

  redisReply *r = execArgv(ctx_, {"SMEMBERS", "online_users"});
  if (!checkReply(r, "SMEMBERS online_users"))
    return users;

  if (r->type == REDIS_REPLY_ARRAY) {
    for (size_t i = 0; i < r->elements; ++i) {
      if (r->element[i]->type == REDIS_REPLY_STRING && r->element[i]->str) {
        std::string name = r->element[i]->str;
        users[name] = getUserGateway(name);
      }
    }
  }
  freeReplyObject(r);
  return users;
}

// 推送消息到用户队列
// target: 目标用户名
// msg: 消息内容
// 返回值: 成功返回true
bool RedisClient::pushMessage(const std::string &target,
                              const std::string &msg) {
  if (!ctx_) {
    std::cerr << "RedisClient: Not connected for pushMessage" << std::endl;
    return false;
  }

  redisReply *r = execArgv(ctx_, {"RPUSH", queueKey(target), msg});
  if (!checkReply(r, "RPUSH msg_queue"))
    return false;
  freeReplyObject(r);
  return true;
}

// 从用户队列中取出所有消息
// target: 目标用户名
// 返回值: 消息列表
std::vector<std::string> RedisClient::popMessages(const std::string &target) {
  std::vector<std::string> msgs;
  if (!ctx_)
    return msgs;

  // 循环取出队列中的所有消息
  while (true) {
    redisReply *r = execArgv(ctx_, {"LPOP", queueKey(target)});
    if (!r || r->type != REDIS_REPLY_STRING) {
      if (r)
        freeReplyObject(r);
      break;
    }
    msgs.emplace_back(r->str, r->len);
    freeReplyObject(r);
  }
  return msgs;
}