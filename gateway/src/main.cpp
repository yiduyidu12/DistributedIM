// DistributedIM Gateway - 分布式即时通讯网关服务器
// 主入口函数，负责初始化配置、日志和启动服务器

#include "EpollServer.h"
#include "Logger.h"
#include "Config.h"

// 主函数
// 参数 argc: 命令行参数数量
// 参数 argv: 命令行参数数组
// 返回值: 程序退出码（0表示成功，非0表示失败）
int main(int argc, char* argv[]) {
    // 步骤1：加载配置文件
    // 优先尝试加载 INI 配置文件
    bool config_loaded = Config::load("../config.ini");
    
    // 如果 INI 加载失败，尝试 JSON 配置文件
    if (!config_loaded) {
        Config::load("../config.json");
    }
    
    // 步骤2：初始化日志系统
    Logger::init(Config::log().file, Config::log().level);
    Logger::info("Starting DistributedIM Gateway...");
    
    // 步骤3：获取端口配置
    int port = Config::server().port;
    
    // 如果命令行传入了端口参数，使用传入的端口
    if (argc >= 2) {
        try {
            int cmd_port = std::stoi(argv[1]);
            if (cmd_port > 0 && cmd_port <= 65535) {
                port = cmd_port;
                Logger::info("Using command line port: {}", port);
            } else {
                Logger::warn("Invalid port range: {}, using config port: {}", argv[1], port);
            }
        } catch (const std::exception& e) {
            Logger::warn("Invalid port format: {}, using config port: {}", argv[1], port);
        }
    }
    
    // 步骤4：启动服务器
    try {
        EpollServer server(port);
        server.start();
    } catch (const std::exception& e) {
        Logger::critical("Server failed to start: {}", e.what());
        return 1;
    }
    
    return 0;
}