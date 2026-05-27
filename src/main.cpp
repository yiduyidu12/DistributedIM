#include "EpollServer.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // 默认端口 8888
    int port = 8888;

    // 如果命令行传入了端口参数，使用传入的端口
    if (argc >= 2) {
        try {
            port = std::stoi(argv[1]); // 使用 std::stoi
            if (port <= 0 || port > 65535) { // 端口范围检查
                std::cerr << "Invalid port range: " << argv[1] << ". Port must be between 1 and 65535." << std::endl;
                return 1;
            }
        } catch (const std::invalid_argument& e) {
            std::cerr << "Invalid port format: " << argv[1] << ". Please provide a valid number." << std::endl;
            return 1;
        } catch (const std::out_of_range& e) {
            std::cerr << "Port number out of range: " << argv[1] << ". Please provide a number between 1 and 65535." << std::endl;
            return 1;
        }
    }
    std::cout << "Starting gateway on port " << port << std::endl;
    EpollServer server(port);
    server.start();
    return 0;
}