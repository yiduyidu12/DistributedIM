# DistributedIM - 分布式即时通讯系统

一个基于 C++ 的高性能分布式即时通讯网关系统，支持多用户在线聊天、私聊、广播、群组等功能。

## 技术栈

- **语言**: C++23
- **网络框架**: 基于 Epoll 的事件驱动模型（Reactor模式）
- **日志系统**: spdlog（header-only）
- **JSON解析**: nlohmann/json（header-only）
- **数据库**: Redis（用户状态管理、消息队列、服务发现）
- **加密**: libsodium（E2EE端到端加密）
- **HTTP客户端**: libcurl（AI服务调用）
- **测试框架**: Google Test

## 项目结构

```
DistributedIM/
├── gateway/                 # 网关核心模块
│   ├── include/             # 头文件
│   │   ├── AckTracker.h     # ACK消息确认追踪器
│   │   ├── AIServiceClient.h # AI服务客户端
│   │   ├── AuditLogger.h    # 审计日志记录器
│   │   ├── Config.h         # 配置管理类
│   │   ├── Connection.h     # 连接状态管理
│   │   ├── EpollServer.h    # Epoll服务器核心
│   │   ├── E2EECrypto.h     # 端到端加密模块
│   │   ├── GatewayRegistry.h # 网关注册与发现
│   │   ├── GroupManager.h   # 群组管理器
│   │   ├── JsonParser.h     # JSON解析工具
│   │   ├── Logger.h         # 日志工具类
│   │   ├── MessageHandler.h # 消息处理器
│   │   ├── Metrics.h        # Prometheus监控指标
│   │   ├── PriorityQueue.h  # 优先级消息队列
│   │   ├── RateLimiter.h    # 令牌桶限流器
│   │   ├── RedisClient.h    # Redis客户端封装
│   │   └── WebSocketCodec.h # WebSocket协议编解码器
│   ├── src/                 # 源文件
│   │   ├── main.cpp         # 程序入口
│   │   └── *.cpp            # 各模块实现
│   ├── tests/               # 单元测试（Google Test）
│   ├── tools/               # 工具程序
│   │   ├── test_client.cpp       # 测试客户端
│   │   ├── test_config_load.cpp  # 配置加载测试
│   │   └── test_config_error.cpp # 配置错误处理测试
│   ├── config.json          # JSON格式配置文件
│   ├── config.ini           # INI格式配置文件（可选）
│   └── CMakeLists.txt       # 网关模块构建配置
├── services/                # 服务模块
│   ├── http-api/            # HTTP REST API服务（Go）
│   ├── ai-service/          # AI服务（Python）
│   └── file-service/        # 文件存储服务（Go）
├── deploy/                  # 部署配置
│   ├── docker-compose.yml   # Docker Compose配置
│   ├── Dockerfile.gateway   # 网关Docker镜像
│   ├── nginx/               # Nginx配置
│   └── prometheus/          # Prometheus监控配置
├── frontend/                # 前端项目（Vue.js）
├── proto/                   # 协议定义
├── scripts/                 # 辅助脚本
├── CMakeLists.txt           # 根目录构建配置
├── .gitignore               # Git忽略规则
└── README.md                # 项目说明文档
```

## 快速开始

### 环境要求

- CMake 3.10+
- GCC 13+ 或 Clang 14+
- Redis 5.0+
- hiredis 库
- libsodium（可选，E2EE加密）
- libcurl（可选，AI服务）

### 安装依赖

```bash
# Ubuntu/Debian
sudo apt-get install cmake g++ libhiredis-dev libsodium-dev libcurl4-openssl-dev redis-server

# 启动 Redis 服务
sudo systemctl start redis-server
```

### 编译项目

**方式一：只编译网关服务器**

```bash
cd gateway/build
cmake ..
make -j4
```

**方式二：编译网关服务器+单元测试**

```bash
cd gateway/build
cmake .. -DBUILD_TESTS=ON
make -j4
```

### 运行服务器

```bash
# 进入 gateway 构建目录
cd gateway/build

# 启动网关服务器（默认使用 config.json 配置）
./gateway_server

# 指定配置文件
./gateway_server --config ../config.ini
```

### 运行测试客户端

```bash
# 在另一个终端运行测试客户端
./test_client 127.0.0.1 8888
```

### 运行单元测试

```bash
# 确保已启用 BUILD_TESTS=ON
./gateway_tests
```

## 配置说明

配置文件支持两种格式：
- `config.json` - JSON格式（默认使用）
- `config.ini` - INI格式

### 主要配置项

```ini
[server]
port = 8888              # 服务端口
max_connections = 1024   # 最大连接数
timeout_seconds = 300    # 连接超时时间(秒)
gateway_id = 1           # 网关ID（多网关部署时需唯一）

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

[e2ee]
enabled = false          # 是否启用端到端加密

[rate_limit]
enabled = true           # 是否启用限流
tokens_per_second = 100  # 每秒生成令牌数
max_burst = 500          # 最大突发令牌数

[metrics]
enabled = true           # 是否启用Prometheus监控
metrics_port = 9090      # 监控指标端口
```

## 功能特性

### 核心功能
- ✅ TCP 长连接管理
- ✅ 用户登录/登出（支持多设备登录）
- ✅ 心跳检测
- ✅ 私聊消息
- ✅ 广播消息
- ✅ 在线用户列表查询
- ✅ 多网关消息路由

### 扩展功能
- ✅ WebSocket协议支持
- ✅ 群组管理（创建、加入、离开、解散）
- ✅ ACK可靠投递（指数退避重试）
- ✅ 令牌桶限流
- ✅ 优先级消息队列
- ✅ 端到端加密（E2EE）
- ✅ AI服务集成（聊天、摘要、情感分析）
- ✅ Prometheus监控指标
- ✅ 网关自动发现与负载均衡
- ✅ 审计日志记录

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
```json
{"type":"who"}
```

### 群组创建
```json
{"type":"group_create","group_name":"mygroup"}
```

### 群组加入
```json
{"type":"group_join","group_name":"mygroup"}
```

### 群组消息
```json
{"type":"group_send","group_name":"mygroup","msg":"Hello group"}
```

## 模块说明

| 模块 | 功能 | 状态 |
|------|------|------|
| EpollServer | 核心事件驱动服务器 | ✅ 完成 |
| RedisClient | Redis客户端封装（用户管理、消息队列） | ✅ 完成 |
| Connection | 客户端连接状态管理 | ✅ 完成 |
| MessageHandler | 消息路由与分发 | ✅ 完成 |
| AckTracker | 消息ACK确认与重试 | ✅ 完成 |
| RateLimiter | 令牌桶限流 | ✅ 完成 |
| GroupManager | 群组管理 | ✅ 完成 |
| WebSocketCodec | WebSocket帧编解码 | ✅ 完成 |
| E2EECrypto | 端到端加密（Ed25519/X25519） | ✅ 完成 |
| GatewayRegistry | 网关注册与服务发现 | ✅ 完成 |
| Metrics | Prometheus监控指标 | ✅ 完成 |
| PriorityQueue | 优先级消息队列 | ✅ 完成 |
| AIServiceClient | AI服务调用客户端 | ✅ 完成 |
| AuditLogger | 审计日志记录 | ✅ 完成 |

## 日志级别说明

| 级别 | 描述 | 适用场景 |
|:---|:---|:---|
| trace | 最详细的追踪信息 | 开发调试 |
| debug | 关键节点信息 | 测试环境 |
| info | 重要业务事件 | 生产环境 |
| warn | 异常情况警告 | 生产环境 |
| error | 错误信息 | 生产环境 |
| critical | 严重错误 | 生产环境 |

## 开发流程

1. **修改代码** → 在 `gateway/include/` 和 `gateway/src/` 目录下修改
2. **编译** → `cd gateway/build && make -j4`
3. **测试** → 运行单元测试或工具程序
4. **提交** → `git add -A && git commit -m "描述"`
5. **推送** → `git push origin main`

## 许可证

MIT License