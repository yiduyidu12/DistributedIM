#include <gtest/gtest.h>
#include "../include/Config.h"

TEST(ConfigTest, DefaultConfig) {
    EXPECT_EQ(Config::server().port, 8888);
    EXPECT_EQ(Config::server().max_connections, 1024);
    EXPECT_EQ(Config::server().timeout_seconds, 300);
    
    EXPECT_EQ(Config::redis().host, "127.0.0.1");
    EXPECT_EQ(Config::redis().port, 6379);
    EXPECT_EQ(Config::redis().password, "");
    EXPECT_EQ(Config::redis().db, 0);
    
    EXPECT_EQ(Config::log().level, "info");
    EXPECT_EQ(Config::log().file, "gateway.log");
}

TEST(ConfigTest, LoadConfig) {
    bool result = Config::load("../config.json");
    EXPECT_TRUE(result);
    
    EXPECT_EQ(Config::server().port, 8888);
    EXPECT_EQ(Config::redis().host, "127.0.0.1");
}

TEST(ConfigTest, LoadNonExistentConfig) {
    bool result = Config::load("../non_existent.json");
    EXPECT_FALSE(result);
}
