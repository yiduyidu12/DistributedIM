#pragma once
#include <string>
#include <ctime>

class Connection {
public:
    int fd_;
    std::string username;
    bool isLogin;//判断是否登入
    time_t last_active;

    Connection(int fd)
        : fd_(fd), isLogin(false), last_active(time(nullptr)) {}
};