// Config 类单元测试
// 使用 Google Test 框架测试配置管理类的功能

#include <gtest/gtest.h>
#include "../include/Config.h"

// 测试默认配置值
// 验证 Config 类的静态成员变量是否正确初始化
TEST(ConfigTest, DefaultConfig) {
    // 验证服务器默认配置
    EXPECT_EQ(Config::server().port, 8888);
    EXPECT_EQ(Config::server().max_connections, 1024);
    EXPECT_EQ(Config::server().timeout_seconds, 300);
    
    // 验证 Redis 默认配置
    EXPECT_EQ(Config::redis().host, "127.0.0.1");
    EXPECT_EQ(Config::redis().port, 6379);
    EXPECT_EQ(Config::redis().password, "");
    EXPECT_EQ(Config::redis().db, 0);
    
    // 验证日志默认配置
    EXPECT_EQ(Config::log().level, "info");
    EXPECT_EQ(Config::log().file, "gateway.log");
}

// 测试配置文件加载
// 验证从 JSON 配置文件加载配置的功能
TEST(ConfigTest, LoadConfig) {
    bool result = Config::load("../config.json");
    EXPECT_TRUE(result);
    
    // 验证加载后的配置值（应与配置文件一致）
    EXPECT_EQ(Config::server().port, 8888);
    EXPECT_EQ(Config::redis().host, "127.0.0.1");
}

// 测试加载不存在的配置文件
// 验证配置文件不存在时的错误处理
TEST(ConfigTest, LoadNonExistentConfig) {
    bool result = Config::load("../non_existent.json");
    EXPECT_FALSE(result);
    // 加载失败后应保持默认配置不变
}