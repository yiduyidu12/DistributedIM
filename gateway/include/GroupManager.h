// GroupManager - 群组聊天管理器
// 负责群组的创建、加入、离开、解散以及群组消息的路由分发
// 群组数据存储在 Redis 中，结构为：
//   room:{id} (Hash) - 群组元数据（名称、创建者、创建时间）
//   room:{id}:members (Set) - 群组成员集合

#ifndef GROUP_MANAGER_H
#define GROUP_MANAGER_H

#include <string>
#include <vector>
#include <functional>

class RedisClient;

class GroupManager {
public:
    // 构造函数
    // 参数 redis: Redis 客户端引用，用于群组数据的持久化操作
    explicit GroupManager(RedisClient& redis);

    // 创建群组
    // 参数 group_name: 群组名称
    // 参数 creator: 创建者用户名
    // 返回值: 成功返回群组ID，失败返回空字符串
    std::string createGroup(const std::string& group_name, const std::string& creator);

    // 加入群组
    // 参数 group_id: 群组ID
    // 参数 username: 要加入的用户名
    // 返回值: 成功返回true，失败返回false（群组不存在/已在群中）
    bool joinGroup(const std::string& group_id, const std::string& username);

    // 离开群组
    // 参数 group_id: 群组ID
    // 参数 username: 要离开的用户名
    // 返回值: 成功返回true，失败返回false
    bool leaveGroup(const std::string& group_id, const std::string& username);

    // 解散群组
    // 参数 group_id: 群组ID
    // 参数 requester: 请求者用户名（必须是创建者）
    // 返回值: 成功返回true，失败返回false
    bool dissolveGroup(const std::string& group_id, const std::string& requester);

    // 获取群组成员列表
    // 参数 group_id: 群组ID
    // 返回值: 成员用户名列表
    std::vector<std::string> getMembers(const std::string& group_id);

    // 检查用户是否为群组成员
    // 参数 group_id: 群组ID
    // 参数 username: 用户名
    // 返回值: 是成员返回true
    bool isMember(const std::string& group_id, const std::string& username);

    // 获取用户加入的所有群组
    // 参数 username: 用户名
    // 返回值: 群组ID列表
    std::vector<std::string> getUserGroups(const std::string& username);

    // 生成新的群组ID
    // 使用 Redis INCR 原子递增生成唯一ID
    // 返回值: 格式为 "g_N" 的群组ID
    std::string generateGroupId();

private:
    RedisClient& redis_;  // Redis 客户端引用
};

#endif // GROUP_MANAGER_H
