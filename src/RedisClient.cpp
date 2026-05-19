#include "RedisClient.h"
#include <iostream>
#include <cstdlib>

RedisClient::RedisClient() : ctx_(nullptr), gateway_id_(rand() % 10000) {}

RedisClient::~RedisClient() { disconnect(); }

bool RedisClient::connect(const std::string& host, int port) {
    ctx_ = redisConnect(host.c_str(), port);
    if (!ctx_ || ctx_->err) {
        if (ctx_) std::cerr << "Redis error: " << ctx_->errstr << std::endl;
        else std::cerr << "Redis connection failed" << std::endl;
        return false;
    }
    std::cout << "[Redis] Connected to Redis server" << std::endl;
    return true;
}

void RedisClient::disconnect() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool RedisClient::userLogin(const std::string& username, int gateway_id, int fd) {
    if (!ctx_) return false;
    redisReply* r = (redisReply*)redisCommand(ctx_,
        "HSET user:%s gateway %d fd %d", username.c_str(), gateway_id, fd);
    if (r) freeReplyObject(r);
    r = (redisReply*)redisCommand(ctx_, "SADD online_users %s", username.c_str());
    if (r) freeReplyObject(r);
    return true;
}

bool RedisClient::userLogout(const std::string& username) {
    if (!ctx_) return false;
    redisReply* r = (redisReply*)redisCommand(ctx_, "DEL user:%s", username.c_str());
    if (r) freeReplyObject(r);
    r = (redisReply*)redisCommand(ctx_, "SREM online_users %s", username.c_str());
    if (r) freeReplyObject(r);
    return true;
}

int RedisClient::getUserGateway(const std::string& username) {
    if (!ctx_) return -1;
    redisReply* r = (redisReply*)redisCommand(ctx_, "HGET user:%s gateway", username.c_str());
    int g = -1;
    if (r && r->type == REDIS_REPLY_STRING) g = std::stoi(r->str);
    if (r) freeReplyObject(r);
    return g;
}

int RedisClient::getUserFd(const std::string& username) {
    if (!ctx_) return -1;
    redisReply* r = (redisReply*)redisCommand(ctx_, "HGET user:%s fd", username.c_str());
    int fd = -1;
    if (r && r->type == REDIS_REPLY_STRING) fd = std::stoi(r->str);
    if (r) freeReplyObject(r);
    return fd;
}

std::unordered_map<std::string, int> RedisClient::getAllOnlineUsers() {
    std::unordered_map<std::string, int> users;
    if (!ctx_) return users;
    redisReply* r = (redisReply*)redisCommand(ctx_, "SMEMBERS online_users");
    if (r && r->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < r->elements; ++i) {
            std::string name = r->element[i]->str;
            users[name] = getUserGateway(name);
        }
    }
    if (r) freeReplyObject(r);
    return users;
}

bool RedisClient::pushMessage(const std::string& target, const std::string& msg) {
    if (!ctx_) return false;
    redisReply* r = (redisReply*)redisCommand(ctx_, "RPUSH msg_queue:%s %s", target.c_str(), msg.c_str());
    if (r) freeReplyObject(r);
    return true;
}

std::vector<std::string> RedisClient::popMessages(const std::string& target) {
    std::vector<std::string> msgs;
    if (!ctx_) return msgs;
    while (true) {
        redisReply* r = (redisReply*)redisCommand(ctx_, "LPOP msg_queue:%s", target.c_str());
        if (!r || r->type != REDIS_REPLY_STRING) {
            if (r) freeReplyObject(r);
            break;
        }
        msgs.push_back(r->str);
        freeReplyObject(r);
    }
    return msgs;
}