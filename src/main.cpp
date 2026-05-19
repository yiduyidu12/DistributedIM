#include "EpollServer.h"
#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    // 默认端口 8888
    int port = 8888;

    // 如果命令行传入了端口参数，使用传入的端口
    if (argc >= 2) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port: " << argv[1] << std::endl;
            return 1;
        }
    }
    std::cout << "Starting gateway on port " << port << std::endl;
    EpollServer server(port);
    server.start();
    return 0;
}