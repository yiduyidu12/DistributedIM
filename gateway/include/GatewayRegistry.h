// GatewayRegistry - 网关自动发现与负载均衡
// 通过 Redis 实现网关注册中心：健康上报、服务发现、区域路由
// 支持 SO_REUSEPORT 多进程部署和地理感知路由

#ifndef GATEWAY_REGISTRY_H
#define GATEWAY_REGISTRY_H

#include <string>
#include <vector>
#include <unordered_map>
#include <ctime>
#include <atomic>
#include <thread>

class RedisClient;

// 网关节点信息
struct GatewayNode {
    std::string gateway_id;    // 网关唯一ID（格式: "gw_<pid>_<port>"）
    std::string host;          // 主机地址
    int         port;          // 监听端口
    std::string region;        // 区域标识（如 "east", "west", "default"）
    int         connections;   // 当前连接数
    double      load;          // 负载系数（0.0 ~ 1.0）
    bool        healthy;       // 是否健康
    time_t      last_heartbeat;  // 最后心跳时间
    time_t      started_at;    // 启动时间

    // 序列化为 JSON（用于 Redis 存储）
    std::string toJson() const;

    // 从 JSON 反序列化
    static GatewayNode fromJson(const std::string& json);

    // 获取 Redis Hash key
    static std::string redisKey(const std::string& gateway_id);
};

// 网关自动发现管理器
// 负责网关注册、心跳上报、服务发现、负载均衡选择
class GatewayRegistry {
public:
    // 构造函数
    // 参数 redis: Redis 客户端引用
    // 参数 gateway_id: 当前网关ID
    // 参数 region: 当前网关所在区域
    explicit GatewayRegistry(RedisClient& redis,
                              const std::string& gateway_id,
                              const std::string& region = "default");

    // 析构函数
    // 停止心跳线程并从注册中心注销
    ~GatewayRegistry();

    // ============ 注册与发现 ============

    // 向 Redis 注册中心注册当前网关
    // 参数 host: 网关主机地址
    // 参数 port: 监听端口
    // 返回值: 注册成功返回true
    bool registerGateway(const std::string& host, int port);

    // 从注册中心注销（优雅关闭时调用）
    // 从 active_gateways 集合中移除并为写入缓冲区排空留出时间
    // 返回值: 注销成功返回true
    bool unregisterGateway();

    // 启动心跳上报线程
    // 参数 interval_seconds: 心跳间隔（默认10秒）
    void startHeartbeat(int interval_seconds = 10);

    // 停止心跳线程
    void stopHeartbeat();

    // 发送一次心跳（更新 last_heartbeat 和负载信息）
    // 返回值: 心跳发送成功返回true
    bool sendHeartbeat();

    // ============ 服务发现 ============

    // 获取所有活跃网关列表
    // 只返回最近心跳正常的网关（last_heartbeat 在 30 秒内）
    // 返回值: 活跃网关节点列表
    std::vector<GatewayNode> discoverGateways();

    // 获取指定区域内的活跃网关
    // 参数 region: 区域标识
    // 返回值: 该区域的活跃网关列表
    std::vector<GatewayNode> discoverGatewaysByRegion(const std::string& region);

    // 获取同区域内的最优网关（负载最低）
    // 用于新连接的路由决策
    // 返回值: 最优网关节点，无可用时 gateway_id 为空
    GatewayNode selectBestGateway();

    // 获取指定区域的网关数量
    // 返回值: 活跃网关数
    int gatewayCount();

    // ============ 负载信息 ============

    // 更新当前网关的负载信息
    // 参数 connections: 当前连接数
    // 参数 load: CPU 负载或自定义负载系数
    void updateLoad(int connections, double load);

    // 计算负载系数（基于连接数和 CPU）
    // 参数 connections: 当前连接数
    // 参数 max_connections: 最大连接数
    // 返回值: 负载系数（0.0 ~ 1.0）
    static double calculateLoad(int connections, int max_connections);

    // ============ 区域路由 ============

    // 判断用户是否在同一区域
    // 通过 Redis 中存储的用户所在网关的区域信息判断
    // 参数 username: 用户名
    // 返回值: 同区域返回true
    bool isSameRegion(const std::string& username);

    // 获取当前网关区域
    // 返回值: 区域标识字符串
    const std::string& getRegion() const;

private:
    // 心跳后台线程
    void heartbeatLoop(int interval_seconds);

    RedisClient& redis_;       // Redis 客户端引用
    std::string  gateway_id_;  // 当前网关ID
    std::string  region_;      // 当前网关区域
    std::string  host_;        // 主机地址
    int          port_;        // 监听端口

    std::atomic<bool> running_;      // 心跳线程运行标志
    std::atomic<int>  connections_;  // 当前连接数（原子更新）
    std::atomic<double> load_;       // 当前负载系数

    std::thread heartbeat_thread_;   // 心跳线程

    // Redis key 常量
    static constexpr const char* ACTIVE_GATEWAYS_KEY = "active_gateways";
    static constexpr const char* GATEWAY_INFO_PREFIX = "gateway:";
    static constexpr int HEARTBEAT_TIMEOUT_SECONDS = 30;
};

#endif // GATEWAY_REGISTRY_H
