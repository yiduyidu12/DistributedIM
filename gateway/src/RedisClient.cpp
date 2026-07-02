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
// 使用Lua脚本确保原子性，支持多设备登录
// 参数 username: 用户名
// 参数 gateway_id: 网关ID
// 参数 fd: 客户端文件描述符
// 返回值: 1=成功, 0=失败(其他错误), -1=连接错误
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

  // Lua脚本：原子化执行多设备登录操作
  // 用户设备信息存储格式：user:username (Set) → {"gw_1:fd_1", "gw_2:fd_2", ...}
  // online_users集合标记用户是否至少有一个在线设备
  // 每次登录：SADD加入新设备 → 如果是首次登录则SADD到online_users → 返回1
  const char *lua_script =
      "local username = ARGV[1] "
      "local device_key = ARGV[2] .. ':' .. ARGV[3] "
      "local user_key = KEYS[1] "
      "redis.call('SADD', user_key, device_key) "
      "redis.call('SADD', 'online_users', username) "
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

  Logger::info("RedisClient: userLogin succeeded - user '{}', gateway={}, fd={}",
               username, gateway_id, fd);
  return result;
}

// 用户登出（原子操作）
// 使用Lua脚本确保原子性，支持多设备登出
// 参数 username: 用户名
// 参数 gateway_id: 网关ID
// 参数 fd: 客户端文件描述符
// 返回值: 成功返回true，失败返回false
bool RedisClient::userLogout(const std::string &username, int gateway_id, int fd) {
  // 参数校验
  if (!isConnected()) {
    Logger::error("RedisClient: Not connected for userLogout");
    return false;
  }
  if (username.empty()) {
    Logger::error("RedisClient: userLogout called with empty username");
    return false;
  }
  if (fd < 0) {
    Logger::error("RedisClient: userLogout called with invalid fd={}", fd);
    return false;
  }

  // Lua脚本：原子化执行多设备登出操作
  // 1. 从用户设备集合中移除指定设备（gw_id:fd）
  // 2. 检查用户是否还有其他设备在线
  // 3. 如果没有其他设备，从online_users集合中移除用户
  const char *lua_script =
      "local username = ARGV[1] "
      "local device_key = ARGV[2] .. ':' .. ARGV[3] "
      "local user_key = KEYS[1] "
      "redis.call('SREM', user_key, device_key) "
      "if redis.call('SCARD', user_key) == 0 then "
      "  redis.call('SREM', 'online_users', username) "
      "end "
      "return 1";

  // 确保C字符串在EVAL调用期间存活
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

  if (!checkReply(r, "EVAL userLogout")) {
    return false;
  }

  freeReplyObject(r);
  Logger::info("RedisClient: userLogout succeeded - user '{}', gateway={}, fd={}",
               username, gateway_id, fd);
  return true;
}

// 获取用户所在网关（支持多设备，返回第一个设备的网关）
// 参数 username: 用户名
// 返回值: 网关ID，失败返回-1
int RedisClient::getUserGateway(const std::string &username) {
  if (!isConnected())
    return -1;
  if (username.empty()) {
    Logger::error("RedisClient: getUserGateway called with empty username");
    return -1;
  }

  // 获取用户的第一个设备（格式: gateway_id:fd）
  redisReply *r =
      execArgv(ctx_, {"SRANDMEMBER", userKey(username)});
  int g = -1;
  if (r && r->type == REDIS_REPLY_STRING) {
    std::string device_info(r->str);
    size_t colon_pos = device_info.find(':');
    if (colon_pos != std::string::npos) {
      std::string gateway_str = device_info.substr(0, colon_pos);
      try {
        g = std::stoi(gateway_str);
      } catch (const std::invalid_argument &) {
        Logger::warn("RedisClient: Invalid gateway format for user {}: {}", 
                     username, gateway_str);
      } catch (const std::out_of_range &) {
        Logger::warn("RedisClient: Gateway value out of range for user {}: {}", 
                     username, gateway_str);
      }
    }
  } else if (r && r->type == REDIS_REPLY_ERROR) {
    Logger::error("RedisClient: SRANDMEMBER error for user {}: {}", 
                  username, r->str);
  }
  if (r)
    freeReplyObject(r);
  return g;
}

// 获取用户的客户端fd（支持多设备，返回第一个设备的fd）
// 参数 username: 用户名
// 返回值: 文件描述符，失败返回-1
int RedisClient::getUserFd(const std::string &username) {
  if (!isConnected())
    return -1;
  if (username.empty()) {
    Logger::error("RedisClient: getUserFd called with empty username");
    return -1;
  }

  // 获取用户的第一个设备（格式: gateway_id:fd）
  redisReply *r = execArgv(ctx_, {"SRANDMEMBER", userKey(username)});
  int fd = -1;
  if (r && r->type == REDIS_REPLY_STRING) {
    std::string device_info(r->str);
    size_t colon_pos = device_info.find(':');
    if (colon_pos != std::string::npos) {
      std::string fd_str = device_info.substr(colon_pos + 1);
      try {
        fd = std::stoi(fd_str);
      } catch (const std::invalid_argument &) {
        Logger::warn("RedisClient: Invalid fd format for user {}: {}", 
                     username, fd_str);
      } catch (const std::out_of_range &) {
        Logger::warn("RedisClient: FD value out of range for user {}: {}", 
                     username, fd_str);
      }
    }
  } else if (r && r->type == REDIS_REPLY_ERROR) {
    Logger::error("RedisClient: SRANDMEMBER error for user {}: {}", 
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
}// RedisClient - Redis客户端封装类新增方法
// 包含群组管理所需的 Hash/Set 操作方法

#include "RedisClient.h"
#include "Logger.h"
#include <vector>

// ============ Hash 操作（群组元数据） ============

// 设置 Hash 字段
// 参数 key: Redis key
// 参数 field: 字段名
// 参数 value: 字段值
// 返回值: 成功返回true
bool RedisClient::setHashField(const std::string& key, const std::string& field,
                                const std::string& value) {
    if (!isConnected()) {
        Logger::error("RedisClient: Not connected for setHashField");
        return false;
    }
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "HSET %s %s %s", key.c_str(), field.c_str(), value.c_str()));
    if (!r || r->type == REDIS_REPLY_ERROR) {
        Logger::error("RedisClient: HSET failed for key={}, field={}", key, field);
        if (r) freeReplyObject(r);
        return false;
    }
    freeReplyObject(r);
    return true;
}

// 获取 Hash 字段
// 参数 key: Redis key
// 参数 field: 字段名
// 返回值: 字段值，失败返回空字符串
std::string RedisClient::getHashField(const std::string& key, const std::string& field) {
    if (!isConnected()) return "";
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "HGET %s %s", key.c_str(), field.c_str()));
    std::string result;
    if (r && r->type == REDIS_REPLY_STRING) {
        result = std::string(r->str, r->len);
    } else if (r && r->type == REDIS_REPLY_ERROR) {
        Logger::error("RedisClient: HGET failed for key={}, field={}: {}", key, field, r->str);
    }
    if (r) freeReplyObject(r);
    return result;
}

// ============ Set 操作（群组成员） ============

// 向集合中添加元素
// 参数 key: Redis key
// 参数 member: 要添加的元素
// 返回值: 成功返回true
bool RedisClient::setAdd(const std::string& key, const std::string& member) {
    if (!isConnected()) return false;
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "SADD %s %s", key.c_str(), member.c_str()));
    bool ok = r && r->type == REDIS_REPLY_INTEGER;
    if (!ok && r) {
        Logger::error("RedisClient: SADD failed for key={}, member={}", key, member);
    }
    if (r) freeReplyObject(r);
    return ok;
}

// 从集合中移除元素
// 参数 key: Redis key
// 参数 member: 要移除的元素
// 返回值: 成功返回true
bool RedisClient::setRemove(const std::string& key, const std::string& member) {
    if (!isConnected()) return false;
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "SREM %s %s", key.c_str(), member.c_str()));
    bool ok = r && r->type == REDIS_REPLY_INTEGER;
    if (r) freeReplyObject(r);
    return ok;
}

// 检查元素是否在集合中
// 参数 key: Redis key
// 参数 member: 要检查的元素
// 返回值: 在集合中返回true
bool RedisClient::setIsMember(const std::string& key, const std::string& member) {
    if (!isConnected()) return false;
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "SISMEMBER %s %s", key.c_str(), member.c_str()));
    bool ok = r && r->type == REDIS_REPLY_INTEGER && r->integer == 1;
    if (r) freeReplyObject(r);
    return ok;
}

// 获取集合所有成员
// 参数 key: Redis key
// 返回值: 成员列表，失败返回空列表
std::vector<std::string> RedisClient::setMembers(const std::string& key) {
    std::vector<std::string> members;
    if (!isConnected()) return members;

    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "SMEMBERS %s", key.c_str()));
    if (r && r->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < r->elements; ++i) {
            if (r->element[i]->type == REDIS_REPLY_STRING) {
                members.emplace_back(r->element[i]->str, r->element[i]->len);
            }
        }
    } else if (r && r->type == REDIS_REPLY_ERROR) {
        Logger::error("RedisClient: SMEMBERS failed for key={}: {}", key, r->str);
    }
    if (r) freeReplyObject(r);
    return members;
}

// 获取集合大小
// 参数 key: Redis key
// 返回值: 集合元素数量
int RedisClient::setSize(const std::string& key) {
    if (!isConnected()) return 0;
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "SCARD %s", key.c_str()));
    int count = 0;
    if (r && r->type == REDIS_REPLY_INTEGER) {
        count = static_cast<int>(r->integer);
    }
    if (r) freeReplyObject(r);
    return count;
}

// ============ 通用操作 ============

// 删除 key
// 参数 key: Redis key
// 返回值: 成功返回true
bool RedisClient::deleteKey(const std::string& key) {
    if (!isConnected()) return false;
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "DEL %s", key.c_str()));
    bool ok = r && r->type == REDIS_REPLY_INTEGER && r->integer > 0;
    if (r) freeReplyObject(r);
    return ok;
}

// 检查 key 是否存在
// 参数 key: Redis key
// 返回值: 存在返回true
bool RedisClient::keyExists(const std::string& key) {
    if (!isConnected()) return false;
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "EXISTS %s", key.c_str()));
    bool ok = r && r->type == REDIS_REPLY_INTEGER && r->integer == 1;
    if (r) freeReplyObject(r);
    return ok;
}

// 原子递增计数器（用于生成唯一ID）
// 参数 key: Redis key
// 返回值: 递增后的值，失败返回-1
long long RedisClient::incr(const std::string& key) {
    if (!isConnected()) return -1;
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "INCR %s", key.c_str()));
    long long val = -1;
    if (r && r->type == REDIS_REPLY_INTEGER) {
        val = r->integer;
    } else if (r && r->type == REDIS_REPLY_ERROR) {
        Logger::error("RedisClient: INCR failed for key={}: {}", key, r->str);
    }
    if (r) freeReplyObject(r);
    return val;
}
