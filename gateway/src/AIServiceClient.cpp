// AIServiceClient - AI 服务 HTTP 客户端实现
// 通过 curl_multi_socket_action() 集成到 Epoll 事件循环
// 实现非阻塞 AI 调用和自动故障转移

#ifdef AI_SERVICE_ENABLED

#include "AIServiceClient.h"
#include "Logger.h"
#include "Config.h"

#include <sys/epoll.h>
#include <nlohmann/json.hpp>
#include <sstream>

// 解决 conda curl 宏与系统 curl 函数签名冲突
#ifdef curl_multi_setopt
#undef curl_multi_setopt
#endif

// 构造函数
// 初始化 curl multi 句柄
AIServiceClient::AIServiceClient()
    : multi_handle_(nullptr), epfd_(-1), current_provider_(0) {
    // 从配置读取 Provider URL 列表
    provider_urls_ = {Config::ai().api_url};
}

// 析构函数
// 清理所有进行中的请求和 curl 资源
AIServiceClient::~AIServiceClient() {
    if (multi_handle_) {
        // 清理所有 easy 句柄
        for (auto& [curl, req_id] : curl_to_request_) {
            curl_multi_remove_handle(multi_handle_, curl);
            curl_easy_cleanup(curl);
        }
        curl_multi_cleanup(multi_handle_);
        multi_handle_ = nullptr;
    }

    // 清理请求中的 headers
    for (auto& [req_id, req] : pending_requests_) {
        if (req.headers) curl_slist_free_all(req.headers);
    }
}

// 初始化 curl multi 句柄
// 返回值: 成功返回true
bool AIServiceClient::init() {
    multi_handle_ = curl_multi_init();
    if (!multi_handle_) {
        Logger::error("[AI] curl_multi_init 失败");
        return false;
    }
    Logger::info("[AI] AI 服务客户端已初始化");
    return true;
}

// 获取底层 curl multi 句柄
// 返回值: CURLM 句柄
CURLM* AIServiceClient::getMultiHandle() {
    return multi_handle_;
}

// 与 Epoll 集成：注册 AI HTTP 连接的 socket 回调
// 参数 epfd: 主 Epoll 实例的文件描述符
void AIServiceClient::integrateWithEpoll(int epfd) {
    epfd_ = epfd;
    if (!multi_handle_) return;

    // 设置 curl multi 的 socket 回调函数
    curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETFUNCTION,
        [](CURL*, curl_socket_t s, int what, void* userp, void*) -> int {
            auto* self = static_cast<AIServiceClient*>(userp);
            epoll_event ev{};
            ev.data.fd = s;

            if (what == CURL_POLL_IN || what == CURL_POLL_INOUT) {
                ev.events |= EPOLLIN;
            }
            if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT) {
                ev.events |= EPOLLOUT;
            }

            if (what == CURL_POLL_REMOVE) {
                epoll_ctl(self->epfd_, EPOLL_CTL_DEL, s, nullptr);
            } else {
                int op = epoll_ctl(self->epfd_, EPOLL_CTL_ADD, s, &ev);
                if (op == -1 && errno == EEXIST) {
                    epoll_ctl(self->epfd_, EPOLL_CTL_MOD, s, &ev);
                }
            }
            return 0;
        }, this);

    // 设置超时回调
    curl_multi_setopt(multi_handle_, CURLMOPT_TIMERFUNCTION,
        [](CURLM*, long, void*) -> int { return 0; }, nullptr);
}

// 处理 curl multi 事件
// 在 epoll_wait 检测到 AI socket 活跃后调用
// 参数 sockfd: 活跃的 socket fd
// 参数 ev_bitmask: epoll 事件位掩码
void AIServiceClient::performAction(int sockfd, int ev_bitmask) {
    if (!multi_handle_) return;

    int running_handles = 0;
    int flags = 0;
    if (ev_bitmask & EPOLLIN) flags |= CURL_CSELECT_IN;
    if (ev_bitmask & EPOLLOUT) flags |= CURL_CSELECT_OUT;

    curl_multi_socket_action(multi_handle_, sockfd, flags, &running_handles);
    processCompletedRequests();
}

// 检查并调用已完成的请求回调
void AIServiceClient::processCompletedRequests() {
    if (!multi_handle_) return;

    int msgs_left = 0;
    CURLMsg* msg = nullptr;

    while ((msg = curl_multi_info_read(multi_handle_, &msgs_left))) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL* easy = msg->easy_handle;
        auto id_it = curl_to_request_.find(easy);
        if (id_it == curl_to_request_.end()) continue;

        std::string request_id = id_it->second;
        auto req_it = pending_requests_.find(request_id);
        if (req_it == pending_requests_.end()) continue;

        AIRequest& req = req_it->second;

        if (msg->data.result == CURLE_OK) {
            req.callback(true, req.response_body);
        } else {
            // 尝试故障转移
            if (!tryFailover(req)) {
                req.callback(false, curl_easy_strerror(msg->data.result));
            }
        }

        // 清理
        curl_multi_remove_handle(multi_handle_, easy);
        curl_easy_cleanup(easy);
        if (req.headers) curl_slist_free_all(req.headers);
        curl_to_request_.erase(id_it);
        pending_requests_.erase(req_it);
    }
}

// 发送 AI 聊天请求
// 参数 prompt: 用户输入
// 参数 context: 对话上下文（JSON 格式）
// 参数 callback: 完成回调
// 返回值: 请求ID
std::string AIServiceClient::chat(const std::string& prompt,
                                   const std::string& context,
                                   AICallback callback) {
    nlohmann::json body{{"prompt", prompt}, {"context", context}};
    return addRequest(getNextProviderUrl() + "/ai/chat", body.dump(), callback);
}

// 发送摘要请求
// 参数 messages: 要摘要的消息列表
// 参数 callback: 完成回调
// 返回值: 请求ID
std::string AIServiceClient::summarize(const std::string& messages,
                                        AICallback callback) {
    nlohmann::json body{{"messages", messages}};
    return addRequest(getNextProviderUrl() + "/ai/summarize", body.dump(), callback);
}

// 发送情感分析请求
// 参数 text: 要分析的文本
// 参数 callback: 完成回调
// 返回值: 请求ID
std::string AIServiceClient::analyzeSentiment(const std::string& text,
                                               AICallback callback) {
    nlohmann::json body{{"text", text}};
    return addRequest(getNextProviderUrl() + "/ai/sentiment", body.dump(), callback);
}

// 获取 AI 服务健康状态
// 返回值: Provider 名称到健康状态的映射（简化实现）
std::unordered_map<std::string, bool> AIServiceClient::getProviderHealth() {
    std::unordered_map<std::string, bool> health;
    for (size_t i = 0; i < provider_urls_.size(); ++i) {
        health[provider_urls_[i]] = true;
    }
    return health;
}

// ============ 内部方法 ============

// HTTP 响应写回调（libcurl）
// 参数 ptr: 数据指针
// 参数 size: 元素大小
// 参数 nmemb: 元素数量
// 参数 userdata: AIRequest 指针
// 返回值: 成功写入的字节数
static size_t writeCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* req = static_cast<AIRequest*>(userdata);
    size_t total = size * nmemb;
    req->response_body.append(static_cast<char*>(ptr), total);
    return total;
}

// 添加新的 curl easy 请求到 multi 句柄
// 参数 url: AI API 端点
// 参数 body: JSON 请求体
// 参数 callback: 完成回调
// 返回值: 请求ID
std::string AIServiceClient::addRequest(const std::string& url,
                                         const std::string& body,
                                         AICallback callback) {
    if (!multi_handle_) {
        Logger::error("[AI] curl multi 句柄未初始化");
        callback(false, "AI client not initialized");
        return "";
    }

    // 生成唯一请求ID
    static int counter = 0;
    std::string request_id = "ai_req_" + std::to_string(++counter);

    auto& req = pending_requests_[request_id];
    req.request_id = request_id;
    req.url = url;
    req.request_body = body;
    req.callback = callback;
    req.retry_count = 0;
    req.done = false;

    // 配置 curl easy 句柄
    CURL* easy = curl_easy_init();
    if (!easy) {
        Logger::error("[AI] curl_easy_init 失败");
        pending_requests_.erase(request_id);
        callback(false, "curl init failed");
        return "";
    }

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &req);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(Config::ai().timeout_ms));

    req.headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, req.headers);

    curl_easy_setopt(easy, CURLOPT_PRIVATE, request_id.c_str());

    curl_multi_add_handle(multi_handle_, easy);
    curl_to_request_[easy] = request_id;

    Logger::trace("[AI] 请求已发送: id={}, url={}", request_id, url);
    return request_id;
}

// 尝试故障转移到下一个 Provider
// 参数 req: 失败的请求
// 返回值: 是否还有备用 Provider
bool AIServiceClient::tryFailover(AIRequest& req) {
    req.retry_count++;
    if (req.retry_count >= static_cast<int>(provider_urls_.size())) {
        Logger::warn("[AI] 所有 Provider 均已失败: id={}", req.request_id);
        return false;
    }

    current_provider_ = (current_provider_ + 1) % provider_urls_.size();
    std::string new_url = getNextProviderUrl() + req.url.substr(req.url.find("/ai/"));
    Logger::info("[AI] 故障转移: id={}, 新Provider={}, 重试#{}/{}",
                 req.request_id, new_url, req.retry_count, provider_urls_.size());

    // 使用新的 URL 重新发起请求
    CURL* easy = curl_easy_init();
    if (!easy) return false;

    req.response_body.clear();
    curl_easy_setopt(easy, CURLOPT_URL, new_url.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, req.request_body.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &req);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(Config::ai().timeout_ms));

    struct curl_slist* new_headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, new_headers);
    if (req.headers) curl_slist_free_all(req.headers);
    req.headers = new_headers;

    curl_multi_add_handle(multi_handle_, easy);
    curl_to_request_[easy] = req.request_id;

    return true;
}

// 获取下一个可用 Provider 的 URL
// 返回值: Provider API 基地址
std::string AIServiceClient::getNextProviderUrl() {
    if (provider_urls_.empty()) return Config::ai().api_url;
    std::string url = provider_urls_[current_provider_];
    current_provider_ = (current_provider_ + 1) % provider_urls_.size();
    return url;
}

#endif // AI_SERVICE_ENABLED
