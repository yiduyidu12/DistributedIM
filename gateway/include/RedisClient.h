// RedisClient - Redis客户端封装类
// 提供分布式用户管理和消息队列功能
// 封装了用户登录/登出、在线状态查询、消息队列操作等原子化操作
// 新增群组管理、优先队列操作支持

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
    void disconnect();

    // 检查是否已连接
    // 返回值: 已连接返回true，未连接返回false
    bool isConnected() const;

    // ============ 用户管理 ============

    // 用户登录（原子操作）
    // 使用Lua脚本确保原子性，避免多网关并发登录竞态
    // 参数 username: 用户名
    // 参数 gateway_id: 网关ID
    // 参数 fd: 客户端文件描述符
    // 返回值: 1=成功, 0=用户已在线(拒绝), -1=错误
    int userLogin(const std::string &username, int gateway_id, int fd);

    // 用户登出（原子操作）
    // 使用Lua脚本确保原子性，支持多设备登出
    // 参数 username: 用户名
    // 参数 gateway_id: 网关ID
    // 参数 fd: 客户端文件描述符
    // 返回值: 成功返回true，失败返回false
    bool userLogout(const std::string &username, int gateway_id, int fd);

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

    // ============ 消息队列 ============

    // 推送消息到用户队列
    // 参数 target: 目标用户名
    // 参数 msg: 消息内容
    // 返回值: 成功返回true，失败返回false
    bool pushMessage(const std::string &target, const std::string &msg);

    // 从用户队列中取出所有消息
    // 参数 target: 目标用户名
    // 返回值: 消息列表
    std::vector<std::string> popMessages(const std::string &target);

    // 获取并清空有待收消息的用户集合
    // 返回值: 有待收消息的用户名列表
    std::vector<std::string> drainPendingUsers();

    // ============ Hash 操作（群组元数据） ============

    // 设置 Hash 字段
    // 参数 key: Redis key
    // 参数 field: 字段名
    // 参数 value: 字段值
    // 返回值: 成功返回true
    bool setHashField(const std::string& key, const std::string& field,
                      const std::string& value);

    // 获取 Hash 字段
    // 参数 key: Redis key
    // 参数 field: 字段名
    // 返回值: 字段值，失败返回空字符串
    std::string getHashField(const std::string& key, const std::string& field);

    // ============ Set 操作（群组成员） ============

    // 向集合中添加元素
    // 参数 key: Redis key
    // 参数 member: 要添加的元素
    // 返回值: 成功返回true
    bool setAdd(const std::string& key, const std::string& member);

    // 从集合中移除元素
    // 参数 key: Redis key
    // 参数 member: 要移除的元素
    // 返回值: 成功返回true
    bool setRemove(const std::string& key, const std::string& member);

    // 检查元素是否在集合中
    // 参数 key: Redis key
    // 参数 member: 要检查的元素
    // 返回值: 在集合中返回true
    bool setIsMember(const std::string& key, const std::string& member);

    // 获取集合所有成员
    // 参数 key: Redis key
    // 返回值: 成员列表
    std::vector<std::string> setMembers(const std::string& key);

    // 获取集合大小
    // 参数 key: Redis key
    // 返回值: 集合元素数量
    int setSize(const std::string& key);

    // ============ 通用操作 ============

    // 删除 key
    // 参数 key: Redis key
    // 返回值: 成功返回true
    bool deleteKey(const std::string& key);

    // 检查 key 是否存在
    // 参数 key: Redis key
    // 返回值: 存在返回true
    bool keyExists(const std::string& key);

    // 原子递增计数器（用于生成唯一ID）
    // 参数 key: Redis key
    // 返回值: 递增后的值，失败返回-1
    long long incr(const std::string& key);

private:
    redisContext *ctx_;  // Redis连接上下文
};

#endif // REDIS_CLIENT_H
