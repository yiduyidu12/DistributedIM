// MessageHandler - 消息处理器类
// 主要功能：注册和分发不同类型的消息处理器

#include "MessageHandler.h"

// 注册消息处理器
// type: 消息类型标识
// handler: 处理函数，接收消息内容和用户名，返回响应消息
void MessageHandler::registerHandler(
    const std::string &type,
    std::function<std::string(const std::string &, const std::string &)> handler) {
  handlers_[type] = std::move(handler);
}

// 处理消息
// type: 消息类型
// msg: 消息内容
// username: 发送消息的用户名
// 返回值: 处理结果（可能为空）
std::string MessageHandler::handle(const std::string &type,
                                   const std::string &msg,
                                   const std::string &username) {
  auto it = handlers_.find(type);
  if (it != handlers_.end()) {
    return it->second(msg, username);
  }
  return "";
}