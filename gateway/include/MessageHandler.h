// MessageHandler - 消息处理器类
// 提供消息类型到处理函数的注册和分发机制
// 支持动态注册不同类型的消息处理器，实现请求分发

#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <functional>
#include <string>
#include <unordered_map>

class MessageHandler {
public:
  // 注册消息处理器
  // 参数 type: 消息类型标识，如"chat"、"send"、"ping"、"who"等
  // 参数 handler: 处理函数，接收消息内容和用户名，返回响应消息
  void registerHandler(
      const std::string &type,
      std::function<std::string(const std::string &, const std::string &)> handler);

  // 处理消息
  // 参数 type: 消息类型
  // 参数 msg: 消息内容（JSON格式字符串）
  // 参数 username: 发送消息的用户名
  // 返回值: 处理结果，为空表示无需响应
  std::string handle(const std::string &type, const std::string &msg,
                     const std::string &username);

private:
  // 消息处理器映射表
  // key: 消息类型字符串
  // value: 对应的处理函数，接收消息内容和用户名，返回响应字符串
  std::unordered_map<std::string, std::function<std::string(const std::string &, const std::string &)>> handlers_;
};

#endif // MESSAGE_HANDLER_H