// RedisClient 类单元测试
// 使用 Google Test 框架测试 Redis 客户端的功能

#include <gtest/gtest.h>
#include "../include/RedisClient.h"
#include "../include/Logger.h"

// RedisClient 测试夹具类
// 提供测试前后的初始化和清理操作
class RedisClientTest : public ::testing::Test {
protected:
    // 测试用 Redis 客户端实例
    RedisClient redis_;
    
    // 测试前的初始化操作
    void SetUp() override {
        // 初始化日志系统
        Logger::init("test.log", "debug");
        // 连接本地 Redis 服务器
        redis_.connect("127.0.0.1", 6379);
    }
    
    // 测试后的清理操作
    void TearDown() override {
        // 确保测试用户已登出，避免影响其他测试
        redis_.userLogout("testuser");
        redis_.userLogout("user1");
        redis_.userLogout("user2");
    }
};

// 测试用户登录功能
// 验证用户登录和网关查询功能
TEST_F(RedisClientTest, UserLogin) {
    // 执行登录操作
    int result = redis_.userLogin("testuser", 1, 100);
    EXPECT_EQ(result, 1);  // 1 表示登录成功
    
    // 验证用户网关信息已正确存储
    int gateway = redis_.getUserGateway("testuser");
    EXPECT_EQ(gateway, 1);
}

// 测试用户登出功能
// 验证用户登出和网关清理功能
TEST_F(RedisClientTest, UserLogout) {
    // 先登录用户
    redis_.userLogin("testuser", 1, 100);
    
    // 执行登出操作
    bool result = redis_.userLogout("testuser");
    EXPECT_TRUE(result);  // 登出成功
    
    // 验证用户网关信息已被清理
    int gateway = redis_.getUserGateway("testuser");
    EXPECT_EQ(gateway, -1);  // -1 表示用户不存在或已离线
}

// 测试消息推送和弹出功能
// 验证消息队列的基本操作
TEST_F(RedisClientTest, PushAndPopMessages) {
    // 推送两条消息到队列
    redis_.pushMessage("testuser", "hello");
    redis_.pushMessage("testuser", "world");
    
    // 弹出所有消息
    auto messages = redis_.popMessages("testuser");
    EXPECT_EQ(messages.size(), 2);  // 应该获取到两条消息
}

// 测试获取在线用户列表功能
// 验证在线用户查询功能
TEST_F(RedisClientTest, GetOnlineUsers) {
    // 登录两个测试用户
    redis_.userLogin("user1", 1, 101);
    redis_.userLogin("user2", 2, 102);
    
    // 获取所有在线用户
    auto users = redis_.getAllOnlineUsers();
    EXPECT_GE(users.size(), 2);  // 至少包含两个测试用户
    
    // 清理测试用户（也会在 TearDown 中执行，这里是为了显式说明）
    redis_.userLogout("user1");
    redis_.userLogout("user2");
}