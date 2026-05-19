#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <unordered_map>
#include <vector>

class RedisClient {
public:
    RedisClient();
    ~RedisClient();

    bool connect(const std::string& host = "127.0.0.1", int port = 6379);
    void disconnect();

    bool userLogin(const std::string& username, int gateway_id, int fd);
    bool userLogout(const std::string& username);
    int getUserGateway(const std::string& username);
    int getUserFd(const std::string& username);
    std::unordered_map<std::string, int> getAllOnlineUsers();

    bool pushMessage(const std::string& target, const std::string& msg);
    std::vector<std::string> popMessages(const std::string& target);

private:
    redisContext* ctx_;
    int gateway_id_;
};