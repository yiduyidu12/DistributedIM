// MessageHandler - 消息处理器类
// 提供消息类型到处理函数的注册和分发机制

#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <functional>
#include <string>
#include <unordered_map>

class MessageHandler {
public:
  // 注册消息处理器
  // type: 消息类型标识
  // handler: 处理函数
  void registerHandler(
      const std::string &type,
      std::function<std::string(const std::string &, const std::string &)> handler);

  // 处理消息
  // type: 消息类型
  // msg: 消息内容
  // username: 发送消息的用户名
  // 返回值: 处理结果
  std::string handle(const std::string &type, const std::string &msg,
                     const std::string &username);

private:
  std::unordered_map<std::string, std::function<std::string(const std::string &, const std::string &)>> handlers_;
};

#endif // MESSAGE_HANDLER_H