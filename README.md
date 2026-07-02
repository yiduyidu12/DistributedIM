# DistributedIM - 分布式即时通讯系统

一个基于 C++ 的高性能分布式即时通讯网关系统，支持多用户在线聊天、私聊、广播等功能。

## 技术栈

- **语言**: C++17
- **网络框架**: 基于 Epoll 的事件驱动模型
- **日志系统**: spdlog
- **配置管理**: nlohmann/json + 自定义 INI 解析器
- **数据库**: Redis (用于用户状态管理和消息队列)
- **测试框架**: Google Test

## 项目结构

```
DistributedIM/
├── gateway/                 # 网关模块
│   ├── include/             # 头文件
│   │   ├── Config.h        # 配置管理类
│   │   ├── Connection.h    # 连接状态管理
│   │   ├── EpollServer.h   # Epoll服务器核心
│   │   ├── JsonParser.h    # JSON解析工具
│   │   ├── Logger.h        # 日志工具类
│   │   ├── MessageHandler.h # 消息处理器
│   │   └── RedisClient.h   # Redis客户端封装
│   ├── src/                # 源文件
│   │   ├── main.cpp        # 程序入口
│   │   └── *.cpp           # 各模块实现
│   ├── tests/              # 单元测试
│   ├── tools/              # 工具程序
│   │   ├── test_client.cpp        # 测试客户端
│   │   ├── test_config_load.cpp   # 配置加载测试
│   │   └── test_config_error.cpp  # 配置错误处理测试
│   ├── config.ini          # INI格式配置文件
│   ├── config.json         # JSON格式配置文件
│   └── CMakeLists.txt      # 构建配置
├── CMakeLists.txt          # 根目录构建配置
├── .gitignore              # Git忽略规则
└── README.md               # 项目说明文档
```

## 快速开始

### 环境要求

- CMake 3.10+
- GCC 7.0+ 或 Clang 6.0+
- Redis 5.0+
- hiredis 库

### 安装依赖

```bash
# Ubuntu/Debian
sudo apt-get install cmake g++ libhiredis-dev redis-server

# 启动 Redis 服务
sudo systemctl start redis-server
```

### 编译项目

```bash
# 创建构建目录
mkdir -p build && cd build

# 配置项目
cmake ..

# 编译
make -j4

# 可选：编译单元测试
cmake .. -DBUILD_TESTS=ON
make -j4
```

### 运行服务器

```bash
# 进入 gateway 构建目录
cd gateway/build

# 启动网关服务器
./gateway_server
```

### 运行测试客户端

```bash
# 在另一个终端运行测试客户端
./test_client 127.0.0.1 8888
```

## 配置说明

配置文件支持两种格式：
- `config.ini` - INI格式，适合人工编辑
- `config.json` - JSON格式，适合程序读写

### 主要配置项

```ini
[server]
port = 8888              # 服务端口
max_connections = 1024   # 最大连接数
timeout_seconds = 300    # 连接超时时间(秒)

[redis]
host = 127.0.0.1        # Redis地址
port = 6379              # Redis端口
password =               # Redis密码(可选)
db = 0                   # 数据库编号

[log]
level = info             # 日志级别: trace, debug, info, warn, error, critical
file = gateway.log       # 日志文件路径
console_output = true    # 是否输出到控制台
file_output = true       # 是否输出到文件
```

## 功能特性

- ✅ TCP 长连接管理
- ✅ 用户登录/登出
- ✅ 心跳检测
- ✅ 私聊消息
- ✅ 广播消息
- ✅ 在线用户列表查询
- ✅ 多网关消息路由
- ✅ 详细日志系统
- ✅ 配置文件管理

## 日志级别说明

| 级别 | 描述 | 适用场景 |
|:---|:---|:---|
| trace | 最详细的追踪信息 | 开发调试 |
| debug | 关键节点信息 | 测试环境 |
| info | 重要业务事件 | 生产环境 |
| warn | 异常情况警告 | 生产环境 |
| error | 错误信息 | 生产环境 |
| critical | 严重错误 | 生产环境 |

## 协议格式

### 登录请求
```json
{"type":"login","username":"user1"}
```

### 心跳请求
```json
{"type":"ping"}
```

### 私聊消息
```json
{"type":"send","to":"user2","msg":"Hello"}
```

### 广播消息
```json
{"type":"chat","msg":"Hello everyone"}
```

### 在线用户查询
```
WHO
```

## 许可证

MIT License
