#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

void sendMessage(int sock, const std::string& type, const std::string& username = "", const std::string& msg = "", const std::string& target = "") {
    std::string json_msg = "{\"type\":\"" + type + "\"";
    if (!username.empty()) {
        json_msg += ",\"username\":\"" + username + "\"";
    }
    if (!msg.empty()) {
        json_msg += ",\"msg\":\"" + msg + "\"";
    }
    if (!target.empty()) {
        json_msg += ",\"to\":\"" + target + "\"";
    }
    json_msg += "}\n";
    
    send(sock, json_msg.c_str(), json_msg.length(), 0);
    std::cout << "[SEND] " << json_msg;
}

void receiveMessage(int sock) {
    char buffer[4096];
    ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        std::cout << "[RECV] " << buffer;
    }
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 8888;  // 使用配置文件中的默认端口
    
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = std::stoi(argv[2]);
    
    std::cout << "=== Test Client for Gateway Server ===" << std::endl;
    std::cout << "Connecting to " << host << ":" << port << std::endl;
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Invalid address" << std::endl;
        close(sock);
        return 1;
    }
    
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "Failed to connect: " << strerror(errno) << std::endl;
        std::cerr << "Make sure the gateway server is running on port " << port << std::endl;
        close(sock);
        return 1;
    }
    
    std::cout << "Connected successfully!" << std::endl;
    std::cout << "=================================" << std::endl;
    
    // Test 1: Heartbeat before login (未登录状态的心跳)
    std::cout << "\n[Test 1] Heartbeat before login..." << std::endl;
    sendMessage(sock, "ping");
    usleep(100000);
    receiveMessage(sock);
    
    // Test 2: Login
    std::cout << "\n[Test 2] Login..." << std::endl;
    sendMessage(sock, "login", "test_user");
    usleep(100000);
    receiveMessage(sock);
    
    // Test 3: Heartbeat after login (登录后的心跳)
    std::cout << "\n[Test 3] Heartbeat after login..." << std::endl;
    sendMessage(sock, "ping");
    usleep(100000);
    receiveMessage(sock);
    
    // Test 4: Multiple heartbeats
    std::cout << "\n[Test 4] Multiple heartbeats (x3)..." << std::endl;
    for (int i = 0; i < 3; ++i) {
        std::cout << "  Heartbeat " << (i+1) << ":" << std::endl;
        sendMessage(sock, "ping");
        usleep(50000);
        receiveMessage(sock);
    }
    
    // Test 5: Chat message
    std::cout << "\n[Test 5] Chat message..." << std::endl;
    sendMessage(sock, "chat", "", "Hello from test client!");
    usleep(100000);
    receiveMessage(sock);
    
    // Test 6: Get online users
    std::cout << "\n[Test 6] Get online users..." << std::endl;
    send(sock, "WHO\n", 4, 0);
    std::cout << "[SEND] WHO\n";
    usleep(100000);
    receiveMessage(sock);
    
    std::cout << "\n=================================" << std::endl;
    std::cout << "All tests completed successfully!" << std::endl;
    std::cout << "Check server logs for detailed logging output." << std::endl;
    
    close(sock);
    return 0;
}
