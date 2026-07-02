// DistributedIM Gateway - 分布式即时通讯网关服务器
// 主入口函数，负责初始化配置、日志和启动服务器

#include "EpollServer.h"
#include "Logger.h"
#include "Config.h"
#include <string>

// 主函数
// 参数 argc: 命令行参数数量
// 参数 argv: 命令行参数数组
// 返回值: 程序退出码（0表示成功，非0表示失败）
int main(int argc, char* argv[]) {
    // 步骤1：解析命令行参数
    std::string config_path;
    int cmd_port = -1;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" || arg == "-c") {
            if (i + 1 < argc) {
                config_path = argv[++i];
                Logger::info("Using config file: {}", config_path);
            } else {
                Logger::warn("Missing config file path after --config");
            }
        } else if (arg == "--port" || arg == "-p") {
            if (i + 1 < argc) {
                try {
                    cmd_port = std::stoi(argv[++i]);
                    if (cmd_port <= 0 || cmd_port > 65535) {
                        Logger::warn("Invalid port range: {}, using config port", cmd_port);
                        cmd_port = -1;
                    }
                } catch (const std::exception&) {
                    Logger::warn("Invalid port format: {}", argv[i]);
                    cmd_port = -1;
                }
            }
        } else if (arg == "--help" || arg == "-h") {
            printf("Usage: gateway_server [options]\n");
            printf("Options:\n");
            printf("  --config, -c <file>   Specify config file path\n");
            printf("  --port, -p <port>     Specify server port\n");
            printf("  --help, -h            Show this help message\n");
            return 0;
        } else {
            // 兼容旧的端口参数格式
            try {
                int port = std::stoi(arg);
                if (port > 0 && port <= 65535) {
                    cmd_port = port;
                    Logger::info("Using command line port: {}", cmd_port);
                }
            } catch (const std::exception&) {
                Logger::warn("Unknown option: {}", arg);
            }
        }
    }
    
    // 步骤2：加载配置文件
    bool config_loaded = false;
    if (!config_path.empty()) {
        config_loaded = Config::load(config_path);
    } else {
        // 优先尝试加载 INI 配置文件
        config_loaded = Config::load("../config.ini");
        // 如果 INI 加载失败，尝试 JSON 配置文件
        if (!config_loaded) {
            config_loaded = Config::load("../config.json");
        }
    }
    
    // 步骤3：初始化日志系统
    Logger::init(Config::log().file, Config::log().level);
    Logger::info("Starting DistributedIM Gateway...");
    
    // 步骤4：获取端口配置
    int port = Config::server().port;
    if (cmd_port > 0) {
        port = cmd_port;
        Logger::info("Using command line port: {}", port);
    }
    
    // 步骤5：启动服务器
    try {
        EpollServer server(port);
        server.start();
    } catch (const std::exception& e) {
        Logger::critical("Server failed to start: {}", e.what());
        return 1;
    }
    
    return 0;
}