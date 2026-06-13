#include <gtest/gtest.h>
#include "../include/JsonParser.h"

TEST(JsonParserTest, GetJsonType) {
    std::string json_str = R"({"type":"login","username":"testuser","password":"123456"})";
    std::string type = getJsonType(json_str);
    
    EXPECT_EQ(type, "login");
}

TEST(JsonParserTest, GetJsonTypeEmpty) {
    std::string type = getJsonType("");
    EXPECT_EQ(type, "");
}

TEST(JsonParserTest, GetJsonString) {
    std::string json_str = R"({"type":"login","username":"testuser","password":"123456"})";
    std::string username = getJsonString(json_str, "username");
    
    EXPECT_EQ(username, "testuser");
}

TEST(JsonParserTest, GetJsonStringMissingKey) {
    std::string json_str = R"({"type":"login","username":"testuser"})";
    std::string missing = getJsonString(json_str, "nonexistent");
    
    EXPECT_EQ(missing, "");
}
