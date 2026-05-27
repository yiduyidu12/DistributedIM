// JsonParser - JSON解析工具类
// 提供从JSON字符串中提取信息的便捷函数

#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <string>

// 从JSON字符串中提取消息类型
// json_str: JSON格式的字符串
// 返回值: 消息类型字符串，失败返回空字符串
std::string getJsonType(const std::string &json_str);

// 从JSON字符串中提取指定字段的值
// json_str: JSON格式的字符串
// key: 要提取的字段名
// 返回值: 字段值，失败返回空字符串
std::string getJsonString(const std::string &json_str, const std::string &key);

#endif // JSON_PARSER_H