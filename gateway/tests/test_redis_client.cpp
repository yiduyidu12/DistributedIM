#include <gtest/gtest.h>
#include "../include/RedisClient.h"
#include "../include/Logger.h"

class RedisClientTest : public ::testing::Test {
protected:
    RedisClient redis_;
    
    void SetUp() override {
        Logger::init("test.log", "debug");
        redis_.connect("127.0.0.1", 6379);
    }
    
    void TearDown() override {
        redis_.userLogout("testuser");
        redis_.userLogout("user1");
        redis_.userLogout("user2");
    }
};

TEST_F(RedisClientTest, UserLogin) {
    int result = redis_.userLogin("testuser", 1, 100);
    EXPECT_EQ(result, 1);
    
    int gateway = redis_.getUserGateway("testuser");
    EXPECT_EQ(gateway, 1);
}

TEST_F(RedisClientTest, UserLogout) {
    redis_.userLogin("testuser", 1, 100);
    
    bool result = redis_.userLogout("testuser");
    EXPECT_TRUE(result);
    
    int gateway = redis_.getUserGateway("testuser");
    EXPECT_EQ(gateway, -1);
}

TEST_F(RedisClientTest, PushAndPopMessages) {
    redis_.pushMessage("testuser", "hello");
    redis_.pushMessage("testuser", "world");
    
    auto messages = redis_.popMessages("testuser");
    EXPECT_EQ(messages.size(), 2);
}

TEST_F(RedisClientTest, GetOnlineUsers) {
    redis_.userLogin("user1", 1, 101);
    redis_.userLogin("user2", 2, 102);
    
    auto users = redis_.getAllOnlineUsers();
    EXPECT_GE(users.size(), 2);
    
    redis_.userLogout("user1");
    redis_.userLogout("user2");
}
