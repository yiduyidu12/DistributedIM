// GatewayRegistry - 网关自动发现与负载均衡实现
// 通过 Redis 实现网关注册中心：健康上报、服务发现、区域路由
// 使用 Hash 存储网关信息、Set 管理活跃网关列表、Lua 脚本保证原子性

#include "GatewayRegistry.h"
#include "RedisClient.h"
#include "Logger.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <chrono>
#include <unistd.h>

// ============ GatewayNode 序列化 ============

// 将网关节点序列化为 JSON
// 返回值: JSON 字符串
std::string GatewayNode::toJson() const {
    nlohmann::json j;
    j["gateway_id"]     = gateway_id;
    j["host"]           = host;
    j["port"]           = port;
    j["region"]         = region;
    j["connections"]    = connections;
    j["load"]           = load;
    j["healthy"]        = healthy;
    j["last_heartbeat"] = static_cast<long long>(last_heartbeat);
    j["started_at"]     = static_cast<long long>(started_at);
    return j.dump();
}

// 从 JSON 字符串反序列化网关节点
// 参数 json: JSON 字符串
// 返回值: 网关节点对象
GatewayNode GatewayNode::fromJson(const std::string& json) {
    auto j = nlohmann::json::parse(json);
    GatewayNode node;
    node.gateway_id     = j.value("gateway_id", "");
    node.host           = j.value("host", "");
    node.port           = j.value("port", 0);
    node.region         = j.value("region", "default");
    node.connections    = j.value("connections", 0);
    node.load           = j.value("load", 0.0);
    node.healthy        = j.value("healthy", true);
    node.last_heartbeat = static_cast<time_t>(j.value("last_heartbeat", 0LL));
    node.started_at     = static_cast<time_t>(j.value("started_at", 0LL));
    return node;
}

// 获取网关在 Redis 中的 Hash key
// 参数 gateway_id: 网关唯一ID
// 返回值: Redis key 字符串（格式: "gateway:gw_xxx"）
std::string GatewayNode::redisKey(const std::string& gateway_id) {
    return std::string("gateway:") + gateway_id;
}

// ============ GatewayRegistry 实现 ============

// 构造函数
// 参数 redis: Redis 客户端引用
// 参数 gateway_id: 当前网关唯一ID
// 参数 region: 区域标识
GatewayRegistry::GatewayRegistry(RedisClient& redis,
                                 const std::string& gateway_id,
                                 const std::string& region)
    : redis_(redis)
    , gateway_id_(gateway_id)
    , region_(region)
    , host_("")
    , port_(0)
    , running_(false)
    , connections_(0)
    , load_(0.0) {
}

// 析构函数
// 停止心跳线程并从注册中心注销
GatewayRegistry::~GatewayRegistry() {
    stopHeartbeat();
    unregisterGateway();
}

// ============ 注册与发现 ============

// 向 Redis 注册中心注册当前网关
// 将网关信息写入 Hash 并加入活跃网关集合
// 参数 host: 网关主机地址
// 参数 port: 监听端口
// 返回值: 注册成功返回true
bool GatewayRegistry::registerGateway(const std::string& host, int port) {
    host_ = host;
    port_ = port;

    GatewayNode node;
    node.gateway_id     = gateway_id_;
    node.host           = host;
    node.port           = port;
    node.region         = region_;
    node.connections    = 0;
    node.load           = 0.0;
    node.healthy        = true;
    node.last_heartbeat = time(nullptr);
    node.started_at     = time(nullptr);

    std::string key   = GatewayNode::redisKey(gateway_id_);
    std::string value = node.toJson();

    // 存储网关完整信息到 Hash
    if (!redis_.setHashField(key, "data", value)) {
        Logger::error("[GatewayRegistry] 注册失败: 无法写入网关信息, id={}", gateway_id_);
        return false;
    }

    // 加入活跃网关集合
    if (!redis_.setAdd(ACTIVE_GATEWAYS_KEY, gateway_id_)) {
        Logger::error("[GatewayRegistry] 注册失败: 无法加入活跃集合, id={}", gateway_id_);
        redis_.deleteKey(key);
        return false;
    }

    Logger::info("[GatewayRegistry] 网关注册成功: id={}, host={}, port={}, region={}",
                 gateway_id_, host, port, region_);
    return true;
}

// 从注册中心注销
// 从活跃集合移除并删除网关信息 Hash
// 返回值: 注销成功返回true
bool GatewayRegistry::unregisterGateway() {
    redis_.setRemove(ACTIVE_GATEWAYS_KEY, gateway_id_);
    redis_.deleteKey(GatewayNode::redisKey(gateway_id_));
    Logger::info("[GatewayRegistry] 网关注销: id={}", gateway_id_);
    return true;
}

// ============ 心跳 ============

// 启动心跳上报线程
// 参数 interval_seconds: 心跳间隔（默认10秒）
void GatewayRegistry::startHeartbeat(int interval_seconds) {
    if (running_.load()) return;

    running_.store(true);
    heartbeat_thread_ = std::thread(&GatewayRegistry::heartbeatLoop, this, interval_seconds);
    Logger::info("[GatewayRegistry] 心跳线程已启动, 间隔={}s", interval_seconds);
}

// 停止心跳线程
void GatewayRegistry::stopHeartbeat() {
    running_.store(false);
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
        Logger::info("[GatewayRegistry] 心跳线程已停止");
    }
}

// 心跳后台循环
// 参数 interval_seconds: 心跳间隔
void GatewayRegistry::heartbeatLoop(int interval_seconds) {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
        if (!running_.load()) break;
        sendHeartbeat();
    }
}

// 发送一次心跳
// 更新 last_heartbeat 时间戳和当前负载信息
// 返回值: 发送成功返回true
bool GatewayRegistry::sendHeartbeat() {
    GatewayNode node;
    node.gateway_id     = gateway_id_;
    node.host           = host_;
    node.port           = port_;
    node.region         = region_;
    node.connections    = connections_.load();
    node.load           = load_.load();
    node.healthy        = true;
    node.last_heartbeat = time(nullptr);
    node.started_at     = time(nullptr);

    std::string key   = GatewayNode::redisKey(gateway_id_);
    std::string value = node.toJson();

    bool ok = redis_.setHashField(key, "data", value);
    if (!ok) {
        Logger::warn("[GatewayRegistry] 心跳发送失败: id={}", gateway_id_);
    }
    return ok;
}

// ============ 服务发现 ============

// 获取所有活跃网关列表
// 通过 SMEMBERS 获取活跃网关ID列表，再逐个 HGET 获取详细信息
// 只返回最近 HEARTBEAT_TIMEOUT_SECONDS 秒内有心跳的网关
// 返回值: 活跃网关节点列表
std::vector<GatewayNode> GatewayRegistry::discoverGateways() {
    std::vector<GatewayNode> result;
    auto members = redis_.setMembers(ACTIVE_GATEWAYS_KEY);
    time_t now = time(nullptr);

    for (const auto& gw_id : members) {
        std::string json = redis_.getHashField(GatewayNode::redisKey(gw_id), "data");
        if (json.empty()) continue;

        GatewayNode node = GatewayNode::fromJson(json);
        // 过滤超时未心跳的网关
        if (now - node.last_heartbeat > HEARTBEAT_TIMEOUT_SECONDS) {
            Logger::trace("[GatewayRegistry] 网关超时剔除: id={}", gw_id);
            redis_.setRemove(ACTIVE_GATEWAYS_KEY, gw_id);
            continue;
        }
        node.healthy = true;
        result.push_back(std::move(node));
    }
    return result;
}

// 获取指定区域内的活跃网关
// 参数 region: 区域标识
// 返回值: 该区域的活跃网关列表
std::vector<GatewayNode> GatewayRegistry::discoverGatewaysByRegion(const std::string& region) {
    std::vector<GatewayNode> result;
    auto all = discoverGateways();
    for (auto& node : all) {
        if (node.region == region) {
            result.push_back(std::move(node));
        }
    }
    return result;
}

// 获取同区域内的最优网关（负载最低）
// 优先选择同区域网关，同区域无可用时回退到所有网关
// 用于新客户端连接的路由决策
// 返回值: 最优网关节点，无可用时 gateway_id 为空
GatewayNode GatewayRegistry::selectBestGateway() {
    // 优先同区域
    auto regional = discoverGatewaysByRegion(region_);
    if (!regional.empty()) {
        // 选择负载最低的网关
        auto best = std::min_element(regional.begin(), regional.end(),
            [](const GatewayNode& a, const GatewayNode& b) {
                return a.load < b.load;
            });
        return *best;
    }

    // 回退到所有区域
    auto all = discoverGateways();
    if (all.empty()) {
        Logger::warn("[GatewayRegistry] 无可用网关");
        return GatewayNode{};
    }

    auto best = std::min_element(all.begin(), all.end(),
        [](const GatewayNode& a, const GatewayNode& b) {
            return a.load < b.load;
        });
    return *best;
}

// 获取指定区域的网关数量
// 返回值: 活跃网关数
int GatewayRegistry::gatewayCount() {
    return static_cast<int>(redis_.setSize(ACTIVE_GATEWAYS_KEY));
}

// ============ 负载信息 ============

// 更新当前网关的负载信息（原子操作）
// 参数 connections: 当前连接数
// 参数 load: CPU 负载或自定义负载系数
void GatewayRegistry::updateLoad(int connections, double load) {
    connections_.store(connections);
    load_.store(load);
}

// 计算负载系数（基于连接数和 CPU）
// 参数 connections: 当前连接数
// 参数 max_connections: 最大连接数
// 返回值: 负载系数（0.0 ~ 1.0，0为空闲，1为满载）
double GatewayRegistry::calculateLoad(int connections, int max_connections) {
    if (max_connections <= 0) return 0.0;
    double ratio = static_cast<double>(connections) / static_cast<double>(max_connections);
    return std::min(ratio, 1.0);
}

// ============ 区域路由 ============

// 判断用户是否在同一区域
// 通过查询用户所在网关的区域信息来判断
// 参数 username: 用户名
// 返回值: 用户与当前网关同区域返回true
bool GatewayRegistry::isSameRegion(const std::string& username) {
    // 查询用户所在网关ID
    int user_gateway = redis_.getUserGateway(username);
    if (user_gateway < 0) return false;

    // 构建用户网关的 Redis key 并获取区域信息
    std::string gw_id = "gw_" + std::to_string(user_gateway);
    std::string json = redis_.getHashField(GatewayNode::redisKey(gw_id), "data");
    if (json.empty()) return false;

    GatewayNode node = GatewayNode::fromJson(json);
    return node.region == region_;
}

// 获取当前网关区域
// 返回值: 区域标识字符串
const std::string& GatewayRegistry::getRegion() const {
    return region_;
}
