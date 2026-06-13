// JsonParser 工具函数单元测试
// 使用 Google Test 框架测试 JSON 解析工具的功能

#include <gtest/gtest.h>
#include "../include/JsonParser.h"

// 测试获取 JSON 消息类型
// 验证 getJsonType 函数能正确提取消息类型字段
TEST(JsonParserTest, GetJsonType) {
    std::string json_str = R"({"type":"login","username":"testuser","password":"123456"})";
    std::string type = getJsonType(json_str);
    
    EXPECT_EQ(type, "login");
}

// 测试空字符串的 JSON 类型提取
// 验证空输入时的边界处理
TEST(JsonParserTest, GetJsonTypeEmpty) {
    std::string type = getJsonType("");
    EXPECT_EQ(type, "");
}

// 测试获取 JSON 字符串字段
// 验证 getJsonString 函数能正确提取指定字段的值
TEST(JsonParserTest, GetJsonString) {
    std::string json_str = R"({"type":"login","username":"testuser","password":"123456"})";
    std::string username = getJsonString(json_str, "username");
    
    EXPECT_EQ(username, "testuser");
}

// 测试获取不存在的 JSON 字段
// 验证缺失字段时的错误处理
TEST(JsonParserTest, GetJsonStringMissingKey) {
    std::string json_str = R"({"type":"login","username":"testuser"})";
    std::string missing = getJsonString(json_str, "nonexistent");
    
    EXPECT_EQ(missing, "");
}