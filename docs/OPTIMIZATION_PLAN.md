# DistributedIM 五大融合方案 - 实施计划

## Context

将现有 C++17 Epoll 分布式 IM 网关升级为综合毕业设计项目，融合五大优化方向：
- **方案一（全栈化）**：WebSocket + React 前端
- **方案二（AI 融合）**：统一 AI 抽象层 + 聊天机器人 + 摘要 + 情感分析
- **方案三（微服务化）**：Docker + 服务拆分 + 可观测性
- **方案四（安全增强）**：JWT + TLS + E2EE + 审计日志
- **方案五（智能路由）**：优先队列 + ACK 可靠投递 + 令牌桶限流

用户拥有 3 个月+时间，前端选 React+TypeScript（我的推荐），AI 采用混合架构（统一抽象层）。

---

## 一、目标架构

```
                        React SPA (Vite + Tailwind + Zustand)
                                │ WSS (TLS 1.3)
                                ▼
                        Nginx (TLS Termination / 路由)
                       ╱        │         ╲
                      ▼         ▼          ▼
              Gateway (C++)  HTTP-API   AI-Service
              Epoll+WS+E2E   (Go/Gin)   (Python/FastAPI)
                      │         │          │
                      └────┬────┘──────────┘
                           ▼
              ┌─────────────────────────┐
              │  Redis 7  +  PostgreSQL  │
              │  (实时状态)  (持久化存储)  │
              └─────────────────────────┘
                           │
              ┌─────────────────────────┐
              │  Prometheus + Grafana   │
              │  可观测性               │
              └─────────────────────────┘
```

**核心设计原则**：
- **保留并扩展**现有 C++ 代码，不重写
- C++ 网关负责实时消息路由，Go 负责 HTTP API，Python 负责 AI
- WebSocket 作为协议升级（同一端口同时支持 TCP 和 WS）
- 所有新功能通过 `registerHandlers()` lambda 注册，保持现有架构模式
- TLS 在 Nginx 层终止，不在 C++ 网关中实现

---

## 二、分阶段实施（13 周）

### Phase 0：基础设施搭建（第 1 周）
- 建立 monorepo 结构（`services/`, `frontend/`, `deploy/`）
- Docker Compose 一键启动：Redis、PostgreSQL、MinIO
- Nginx 配置 TLS + WebSocket 代理
- 环境：`deploy/docker-compose.yml`, `deploy/nginx/nginx.conf`

### Phase 1：WebSocket + 用户系统 + TLS（第 1-3 周）

#### 1A. WebSocket 支持
- **新建**: `gateway/include/WebSocketCodec.h`, `gateway/src/WebSocketCodec.cpp`
- **修改**: `Connection.h` 添加 `is_websocket`、`Protocol` 枚举
- **修改**: `EpollServer.cpp` 的 `handleRead()` 检测 Upgrade 请求
- 实现 RFC 6455 核心帧：Text/Binary/Close/Ping/Pong
- `sendToClient()` 自动根据协议类型包装帧

#### 1B. 用户账户系统 + PostgreSQL
- **新建**: `services/http-api/` Go 服务（Gin 框架）
- bcrypt 密码哈希 + JWT 令牌（15min access + 7day refresh）
- 数据库表：`users`, `messages`
- **修改**: `EpollServer.cpp` 的 `performLogin()` 验证 JWT

#### 1C. TLS 加密
- Nginx 层 TLS 终止（开发环境自签名证书）
- 支持 WSS（WebSocket Secure）和 HTTPS

### Phase 2：核心 IM 增强（第 3-5 周）

#### 2A. 群组聊天
- **新建**: `gateway/include/GroupManager.h`, `gateway/src/GroupManager.cpp`
- Redis 数据结构：`room:{id}` (Hash), `room:{id}:members` (Set)
- 消息类型：`group_send`, `group_join`, `group_leave`, `group_create`
- **修改**: `RedisClient.cpp` 添加群组 Lua 脚本操作

#### 2B. 可靠消息投递（ACK）
- **新建**: `gateway/include/AckTracker.h`, `gateway/src/AckTracker.cpp`
- **修改**: `Connection.h` 添加 `pending_acks_` 映射
- 客户端生成 `msg_id`（UUID），服务端跟踪 ACK 状态
- 指数退避重试：1s, 2s, 4s, 8s（最多 3 次）
- **修改**: `EpollServer.cpp` 的 `loop()` 添加 ACK 重试逻辑

#### 2C. 优先队列 + 令牌桶限流
- **新建**: `gateway/include/RateLimiter.h`, `gateway/include/PriorityQueue.h`
- 三级优先级：URGENT（系统/ACK） > NORMAL（聊天） > BULK（文件/历史）
- 令牌桶：每连接独立限流，默认 10 msg/s
- **修改**: `EpollServer.cpp` 的 `handleMessage()` 添加限流检查

### Phase 3：React 前端（第 5-8 周）

#### 3A. SPA 基础架构
- Vite 5 + React 18 + TypeScript 5 + Tailwind CSS
- Zustand 状态管理（auth/chat/presence/websocket stores）
- reconnecting-websocket 自动重连

#### 3B. 核心页面
- 登录/注册页 → 聊天主界面（3 栏布局）
- 会话列表 | 聊天窗口 | 在线用户侧栏
- 消息气泡（支持 Markdown、Emoji、代码高亮）
- 输入框（@提及自动补全、图片粘贴上传）

#### 3C. 高级交互
- 无限滚动消息历史（TanStack Query + Intersection Observer）
- 用户在线状态（online/away/busy/offline）
- 正在输入指示器（2s 节流）
- 已读回执（双勾 ✓✓）

### Phase 4：AI 融合（第 7-10 周）

#### 4A. 统一 AI 抽象层（核心创新点）
- **新建**: `services/ai-service/` Python FastAPI 服务
- `providers/base.py`：抽象基类（chat/chat_stream/check_health）
- 适配器：Claude、通义千问、DeepSeek、OpenAI-compatible
- 自动故障转移链：Claude → DeepSeek → 通义千问
- **新建**: `gateway/include/AIServiceClient.h`（非阻塞 HTTP）

#### 4B. AI 聊天机器人
- 群聊中 `@bot` 或 `!ai` 触发
- 多轮对话上下文维护（Redis TTL 30min）
- AI 回复以 `from: "ai_bot"` 注入消息流

#### 4C. 离线摘要 + 情感分析
- 用户离线 >1h 后上线，自动生成未读摘要
- 异步情感评分（每条消息），前端颜色渲染
- 智能回复建议（3 条候选）

### Phase 5：安全深化（第 8-10 周）

#### 5A. JWT 认证完善
- Token 刷新轮换（refresh token rotation）
- 网关本地验证 JWT（零 DB 调用）

#### 5B. 端到端加密（E2EE，核心创新点）
- **新建**: `gateway/lib/crypto/` C++ 加密模块（基于 libsodium）
- Ed25519 密钥对 + X25519 DH 密钥交换
- AES-256-GCM 每消息加密
- HKDF 密钥派生 + 单棘轮（DH ratchet）
- 编译可选：`#ifdef E2E_ENABLED`
- 前端对应：`frontend/src/services/crypto.ts`（Web Crypto API）
- **服务端不可解密**，仅路由密文

#### 5C. 审计日志
- **新建**: `gateway/include/AuditLogger.h`
- 记录：登录尝试、消息发送、群组操作、限流触发

### Phase 6：容器化 + 可观测性 + 收尾（第 10-13 周）

#### 6A. Docker Compose 全栈部署
- 8 个服务：nginx, gateway, http-api, ai-service, file-service, redis, postgres, minio
- 多阶段 Dockerfile（C++/Go/Python）
- 一键启动：`docker-compose up -d`

#### 6B. Prometheus + Grafana 监控
- **新建**: `gateway/include/Metrics.h`（Prometheus 格式指标导出）
- 指标：连接数、消息吞吐量、延迟分位数、Redis 操作数、AI 调用量
- Grafana 预置仪表板

#### 6C. 文件/媒体共享
- **新建**: `services/file-service/` Go 服务
- MinIO 对象存储，缩略图生成
- 消息中嵌入文件引用 URL

#### 6D. 地理感知路由
- 网关配置 `region` 标识
- 同区域用户优先本地广播
- 跨区域消息批量合并

#### 6E. 性能评测 + 论文写作
- 8 项基准测试：吞吐量、延迟、WS vs TCP、E2E 开销、AI 质量对比等
- 论文 9 章结构：引言→相关工作→架构→网关设计→分布式路由→AI 集成→安全→评测→结论

---

## 三、关键文件变更清单

### 修改（扩展现有代码）
| 文件 | 变更内容 |
|------|---------|
| `gateway/include/Connection.h` | 添加 `is_websocket`, `pending_acks_`, `RateLimiter` |
| `gateway/include/EpollServer.h` | 添加 WS/AI/E2E/Group 方法声明 |
| `gateway/src/EpollServer.cpp` | `handleRead()` WS 升级检测，`registerHandlers()` 新类型，`loop()` ACK 逻辑，`performLogin()` JWT |
| `gateway/include/RedisClient.h` | 添加群组/优先队列/在线状态方法 |
| `gateway/src/RedisClient.cpp` | 群组 Lua 脚本、优先队列实现 |
| `gateway/include/Config.h` | 添加 E2E/AI/区域配置项 |
| `gateway/src/Config.cpp` | 解析新配置段 |
| `gateway/CMakeLists.txt` | 链接 libsodium、libcurl、新源文件 |

### 新建（从零创建）
- `gateway/include/WebSocketCodec.h` + `.cpp` — RFC 6455 帧编解码
- `gateway/include/AckTracker.h` + `.cpp` — ACK 状态机
- `gateway/include/RateLimiter.h` + `.cpp` — 令牌桶
- `gateway/include/GroupManager.h` + `.cpp` — 群组管理
- `gateway/include/AIServiceClient.h` + `.cpp` — AI HTTP 客户端
- `gateway/include/AuditLogger.h` + `.cpp` — 审计日志
- `gateway/include/Metrics.h` + `.cpp` — Prometheus 指标
- `gateway/lib/crypto/` — E2E 加密模块（libsodium 封装）
- `services/http-api/` — Go REST API（~15 文件）
- `services/ai-service/` — Python AI 服务（~15 文件）
- `frontend/` — React SPA（~30 文件）
- `deploy/` — Docker Compose + Nginx + 监控配置（~10 文件）

---

## 四、论文创新点

1. **统一 AI 抽象层**：多提供商自动故障转移，质量与成本对比
2. **C++/Go/Python 异构微服务**：各取所长的多语言架构
3. **E2E 加密实现**：简化 Signal Protocol 在分布式 IM 中的应用
4. **优先级感知消息路由**：QoS 区分 + 令牌桶限流
5. **地理感知跨网关路由**：批量合并减少跨区域带宽

## 五、🔍 方案审计（2026-06-13 计划模式复查）

对原方案进行系统性审计，发现 **8 个关键漏洞** 和 **3 个技术决策需要细化**。

---

### 审计发现 #1（🔴 严重）：WebSocket 帧解析与现有换行分割逻辑不兼容

**问题**：现有 `handleRead()` 使用换行符 `\n` 分割消息。WebSocket 使用二进制帧头，两者互斥。

**解决方案**：
- Connection 新增 `ws` 子结构体（帧解析状态机 + 重组缓冲区）
- `handleRead()` 重构为三阶段：①协议检测/握手 ②协议特定消息提取（WS帧解析 vs TCP换行分割）③消息分发
- WebSocket 控制帧（Ping/Pong/Close）在帧解析层内联处理，不进入 handleMessage()
- 帧解析状态机：`HEADER_BASIC → HEADER_EXTENDED → HEADER_MASK → PAYLOAD`，可跨 TCP 分段恢复
- 客户端→服务端帧强制检查 MASK 位，未掩码帧返回 1002 协议错误
- 最大帧载荷限制 64KB（与 TCP 行限制一致）

**实现细节**：详见 [WebSocket 帧解析状态机设计](#websocket-帧解析状态机)

---

### 审计发现 #2（🔴 严重）：缺少多设备同步支持

**问题**：当前 `user_map_` 是 `username → fd` 的一对一映射。一个用户只能在一台设备登录，与微信/Discord/Telegram 的多设备现实不符。

**解决方案（新增 Phase 2D）**：
- `user_map_` 改为 `username → std::vector<int>`（一个用户多个 fd）
- 每个设备独立维护自己的 `device_id`（客户端生成 UUID）
- 消息投递改为：查找用户所有在线设备 → 逐个投递
- E2EE 密钥按设备管理（每设备独立密钥对，不跨设备共享棘轮）
- 离线上线设备通过 PostgreSQL 拉取错过的消息历史
- 在线状态聚合：任一设备在线 = 用户在线

---

### 审计发现 #3（🟡 高）：单线程事件循环 CPU 密集型操作风险

**问题**：E2EE 加密（AES-256-GCM + HKDF + DH）、AI HTTP 调用全在主 Epoll 线程，1000 msg/s 时延迟抖动严重。

**解决方案**：
- E2EE 加解密移入独立线程池（4 worker threads + lock-free queue）
- AI HTTP 客户端使用 `curl_multi_socket_action()` 集成到 epoll（不是独立线程）
- 多 worker 进程架构：`SO_REUSEPORT` + 共享 Redis 后端（新增到 Phase 6D）
- 性能测试需测量 P50/P95/P99 延迟分位数

---

### 审计发现 #4（🟡 高）：缺少消息生命周期管理 + 全文搜索

**问题**：消息可以创建但不能编辑/删除，也无法全文搜索。

**解决方案（新增到 Phase 2A + Phase 6）**：
- 消息编辑（append-only，保留历史版本）
- 消息删除（软删除标记）
- PostgreSQL `tsvector` 中文全文搜索（使用 `zhparser` 或 `jieba` 分词）
- 前端搜索 UI（Phase 3 新增搜索组件）

---

### 审计发现 #5（🟡 高）：缺少推送通知与离线机制

**问题**：用户关闭浏览器后完全不知道有新消息。

**解决方案（新增到 Phase 6）**：
- Web Push API 浏览器推送通知
- 离线消息缓存 Redis → 用户上线后批量推送
- 未读计数维护（Redis INCR/DECR）

---

### 审计发现 #6（🟡 中）：缺少对比基准测试

**问题**：原方案的 8 项基准测试都是孤立的数字，没有对比对象。

**解决方案（改进 Phase 6E）**：
- 吞吐量对比：C++ Epoll vs Go gorilla/websocket vs Node.js ws
- 延迟对比：本系统 vs 原生 TCP（无应用层协议）基线
- E2EE 开销：加密 ON vs OFF 的吞吐量/延迟对比
- 故障注入：Redis 断开恢复、网络延迟模拟、CPU 压力测试
- C10K 目标：验证 10000 并发连接下的内存和 CPU

---

### 审计发现 #7（🟡 中）：缺少现代 IM 交互功能

**问题**：前端缺少消息回应（Emoji Reaction）、线程回复、好友系统。

**解决方案（新增到 Phase 3）**：
- **消息 Emoji Reaction**：协议新增 `{"type":"react","msg_id":"...","emoji":"👍"}`
- **消息引用回复**：协议新增 `{"type":"send","reply_to":"msg_id","msg":"..."}`
- **好友系统**：新增 `friend_add`/`friend_accept`/`friend_block` 消息类型
- **用户头像/个人资料**：HTTP API 提供头像上传和资料编辑

---

### 审计发现 #8（🟢 低）：运维韧性不足

**问题**：无健康检查、无优雅关闭、无 schema 迁移、无死信队列。

**解决方案（新增到 Phase 6）**：
- Docker HEALTHCHECK 指令
- 优雅关闭：收到 SIGTERM → 停止 accept → 排空写缓冲 → 刷新 ACK → 断开连接
- PostgreSQL 迁移使用 `golang-migrate`（Go API 服务自带）
- 死信队列：ACK 重试 3 次失败 → Redis `dead_letter:{username}` 列表 → 定期检查
- 限流溢出返回错误消息：`{"type":"error","msg":"rate limit exceeded","retry_after":5}`

---

### 技术决策细化

#### 决策 1：JWT HMAC-SHA256 依赖选择

| 方案 | 评价 |
|:---|:---|
| OpenSSL `libcrypto` | ❌ 太重，仅为一个 HMAC 引入完整 TLS 库 |
| libsodium `crypto_auth_hmacsha256` | ❌ 为单个函数引入整个加密库 |
| **jwt-cpp 头文件库 + nlohmann/json** | ✅ **推荐**，零链接依赖，完整 JWT 管线 |

#### 决策 2：非阻塞 HTTP 客户端

| 方案 | 评价 |
|:---|:---|
| 独立 HTTP 工作线程 + 阻塞 libcurl | ❌ 引入线程安全问题 |
| **`curl_multi_socket_action()` + epoll 集成** | ✅ **推荐**，单线程，无锁，~80 行胶水代码 |

#### 决策 3：AuditLogger 数据通路

| 方案 | 评价 |
|:---|:---|
| 网关直接连接 PostgreSQL | ❌ 破坏服务边界，引入 libpq 依赖 |
| **HTTP POST 到 Go API `/api/audit`** | ✅ **推荐**，保持 C++ 网关无状态 |

---

### 调整后的实施计划（13 周 → 15 周）

| 阶段 | 时间 | 新增内容 |
|:---|:---|:---|
| Phase 0 | 第 1 周 | 不变 |
| Phase 1 | 第 1-3 周 | WS 帧解析状态机（审计 #1）、JWT 用 jwt-cpp |
| Phase 2 | 第 3-6 周 | **+2D 多设备支持**（审计 #2）、消息编辑/删除（审计 #4） |
| Phase 3 | 第 5-9 周 | **+Emoji Reaction + 引用回复 + 好友系统**（审计 #7）、搜索 UI |
| Phase 4 | 第 7-11 周 | curl_multi 集成（审计 #3）、**+数据隐私同意**（审计 #6） |
| Phase 5 | 第 8-11 周 | 加密线程池（审计 #3） |
| Phase 6 | 第 10-15 周 | **+推送通知**（审计 #5）、**+对比基准测试**（审计 #6）、**+运维韧性**（审计 #8） |

---

## 六、验证方案

- **单元测试**：Google Test（C++）+ pytest（Python）+ Go testing
- **集成测试**：Docker Compose 环境端到端消息流测试
- **WebSocket 合规性**：Autobahn Test Suite
- **性能基准**：wrk + 自定义压测工具（100/1K/10K 并发）
- **E2E 正确性**：已知明文→密文→解密→明文循环验证
- **AI 质量**：BLEU 评分 + 人工评估
