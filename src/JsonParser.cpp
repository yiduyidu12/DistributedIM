// JsonParser - JSON解析工具类
// 主要功能：从JSON字符串中提取消息类型和字段值

#include "JsonParser.h"
#include <nlohmann/json.hpp>

// 从JSON字符串中提取消息类型
// json_str: JSON格式的字符串
// 返回值: 成功返回消息类型字符串，失败返回空字符串
std::string getJsonType(const std::string &json_str) {
  try {
    auto j = nlohmann::json::parse(json_str);
    if (j.contains("type") && j["type"].is_string()) {
      return j["type"].get<std::string>();
    }
  } catch (const nlohmann::json::parse_error &) {
    // 解析失败，返回空字符串
  }
  return "";
}

// 从JSON字符串中提取指定字段的值
// json_str: JSON格式的字符串
// key: 要提取的字段名
// 返回值: 成功返回字段值，失败返回空字符串
std::string getJsonString(const std::string &json_str, const std::string &key) {
  try {
    auto j = nlohmann::json::parse(json_str);
    if (j.contains(key) && j[key].is_string()) {
      return j[key].get<std::string>();
    }
  } catch (const nlohmann::json::parse_error &) {
    // 解析失败，返回空字符串
  }
  return "";
}