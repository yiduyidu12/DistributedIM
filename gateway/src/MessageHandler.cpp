// MessageHandler - 消息处理器类
// 提供消息类型到处理函数的注册和分发机制
// 使用unordered_map存储消息类型与处理函数的映射关系

#include "MessageHandler.h"

// 注册消息处理器
// 参数 type: 消息类型标识，如"chat"、"send"、"ping"等
// 参数 handler: 处理函数，接收消息内容和用户名，返回响应消息
void MessageHandler::registerHandler(
    const std::string &type,
    std::function<std::string(const std::string &, const std::string &)> handler) {
  handlers_[type] = std::move(handler);
}

// 处理消息
// 参数 type: 消息类型
// 参数 msg: 消息内容（JSON格式字符串）
// 参数 username: 发送消息的用户名
// 返回值: 处理结果，如果未找到对应的处理器则返回空字符串
std::string MessageHandler::handle(const std::string &type,
                                   const std::string &msg,
                                   const std::string &username) {
  auto it = handlers_.find(type);
  if (it != handlers_.end()) {
    return it->second(msg, username);
  }
  return "";
}