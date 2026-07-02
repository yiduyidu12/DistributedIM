// GroupManager - 群组聊天管理器
// 负责群组的创建、加入、离开、解散以及群组消息路由
// 群组数据存储在 Redis 中

#include "GroupManager.h"
#include "RedisClient.h"
#include "Logger.h"
#include <sstream>
#include <ctime>

// 构造函数
// 参数 redis: Redis 客户端引用
GroupManager::GroupManager(RedisClient& redis) : redis_(redis) {}

// 生成新的群组ID
// 使用 Redis INCR 命令原子递增群组计数器
// 返回值: 格式为 "g_N" 的唯一群组ID
std::string GroupManager::generateGroupId() {
    // 使用 Redis INCR 原子递增，避免并发创建时的ID冲突
    long long id = redis_.incr("group_counter");
    if (id <= 0) {
        Logger::error("[Group] 生成群组ID失败：Redis INCR 返回 {}", id);
        return "";
    }
    return "g_" + std::to_string(id);
}

// 创建群组
// 参数 group_name: 群组名称
// 参数 creator: 创建者用户名
// 返回值: 成功返回群组ID，失败返回空字符串
std::string GroupManager::createGroup(const std::string& group_name,
                                       const std::string& creator) {
    if (group_name.empty() || creator.empty()) {
        Logger::warn("[Group] 创建群组失败：群组名或创建者为空");
        return "";
    }

    std::string group_id = generateGroupId();
    Logger::info("[Group] 创建群组: id={}, name={}, creator={}",
                 group_id, group_name, creator);

    // 存储群组元数据到 Redis Hash
    // room:{id} → {name, creator, created_at}
    std::string room_key = "room:" + group_id;
    redis_.setHashField(room_key, "name", group_name);
    redis_.setHashField(room_key, "creator", creator);
    redis_.setHashField(room_key, "created_at", std::to_string(time(nullptr)));

    // 将创建者加入群组成员集合
    std::string members_key = "room:" + group_id + ":members";
    redis_.setAdd(members_key, creator);

    // 记录用户-群组关系
    redis_.setAdd("user:" + creator + ":groups", group_id);

    Logger::info("[Group] 群组创建成功: id={}, creator={}", group_id, creator);
    return group_id;
}

// 加入群组
// 参数 group_id: 群组ID
// 参数 username: 要加入的用户名
// 返回值: 成功返回true
bool GroupManager::joinGroup(const std::string& group_id, const std::string& username) {
    if (group_id.empty() || username.empty()) {
        Logger::warn("[Group] 加入群组失败：参数无效");
        return false;
    }

    // 检查群组是否存在
    std::string room_key = "room:" + group_id;
    std::string name = redis_.getHashField(room_key, "name");
    if (name.empty()) {
        Logger::warn("[Group] 加入群组失败：群组 '{}' 不存在", group_id);
        return false;
    }

    // 检查是否为重复加入
    std::string members_key = "room:" + group_id + ":members";
    if (redis_.setIsMember(members_key, username)) {
        Logger::warn("[Group] 用户 '{}' 已在群组 '{}' 中", username, group_id);
        return false;
    }

    // 添加到群组成员集合
    redis_.setAdd(members_key, username);

    // 记录用户-群组关系
    redis_.setAdd("user:" + username + ":groups", group_id);

    Logger::info("[Group] 用户 '{}' 加入了群组 '{}'", username, group_id);
    return true;
}

// 离开群组
// 参数 group_id: 群组ID
// 参数 username: 要离开的用户名
// 返回值: 成功返回true
bool GroupManager::leaveGroup(const std::string& group_id, const std::string& username) {
    if (group_id.empty() || username.empty()) return false;

    std::string members_key = "room:" + group_id + ":members";
    redis_.setRemove(members_key, username);
    redis_.setRemove("user:" + username + ":groups", group_id);

    Logger::info("[Group] 用户 '{}' 离开了群组 '{}'", username, group_id);
    return true;
}

// 解散群组
// 参数 group_id: 群组ID
// 参数 requester: 请求者用户名（必须是创建者）
// 返回值: 成功返回true
bool GroupManager::dissolveGroup(const std::string& group_id,
                                  const std::string& requester) {
    std::string room_key = "room:" + group_id;
    std::string creator = redis_.getHashField(room_key, "creator");
    if (creator != requester) {
        Logger::warn("[Group] 解散群组失败：用户 '{}' 不是群组 '{}' 的创建者",
                     requester, group_id);
        return false;
    }

    // 获取所有成员并清理
    auto members = getMembers(group_id);
    for (const auto& member : members) {
        redis_.setRemove("user:" + member + ":groups", group_id);
    }

    // 删除群组数据
    redis_.deleteKey("room:" + group_id + ":members");
    redis_.deleteKey(room_key);

    Logger::info("[Group] 群组 '{}' 已被创建者 '{}' 解散", group_id, requester);
    return true;
}

// 获取群组成员列表
// 参数 group_id: 群组ID
// 返回值: 成员用户名列表
std::vector<std::string> GroupManager::getMembers(const std::string& group_id) {
    return redis_.setMembers("room:" + group_id + ":members");
}

// 检查用户是否为群组成员
// 参数 group_id: 群组ID
// 参数 username: 用户名
// 返回值: 是成员返回true
bool GroupManager::isMember(const std::string& group_id, const std::string& username) {
    return redis_.setIsMember("room:" + group_id + ":members", username);
}

// 获取用户加入的所有群组
// 参数 username: 用户名
// 返回值: 群组ID列表
std::vector<std::string> GroupManager::getUserGroups(const std::string& username) {
    return redis_.setMembers("user:" + username + ":groups");
}
