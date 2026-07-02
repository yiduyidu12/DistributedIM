// AIServiceClient - AI 服务 HTTP 客户端
// 通过 curl_multi_socket_action() 集成到 Epoll 事件循环
// 实现非阻塞 AI 调用（聊天、摘要、情感分析）
// 支持自动故障转移链：Claude → DeepSeek → Qwen（通义千问）

#ifndef AI_SERVICE_CLIENT_H
#define AI_SERVICE_CLIENT_H

#include <string>
#include <functional>
#include <unordered_map>
#include <curl/curl.h>

// AI 回调类型定义
// 参数 success: 是否成功
// 参数 response: AI 返回的响应文本
using AICallback = std::function<void(bool success, const std::string& response)>;

// AI 请求上下文（追踪进行中的请求）
struct AIRequest {
    std::string request_id;     // 请求唯一ID
    std::string url;            // AI 服务 URL
    std::string request_body;   // 请求体（JSON）
    std::string response_body;  // 响应体缓冲区
    AICallback  callback;       // 完成回调
    curl_slist* headers;        // HTTP 请求头
    int         retry_count;    // 故障转移次数
    bool        done;           // 是否已完成
};

// AI 服务客户端
// 非阻塞 HTTP 客户端，集成到 Epoll 事件循环
class AIServiceClient {
public:
    // 构造函数
    AIServiceClient();

    // 析构函数
    // 清理 curl multi 句柄和进行中的请求
    ~AIServiceClient();

    // 初始化 curl multi 句柄
    // 返回值: 成功返回true
    bool init();

    // 获取底层 curl multi 句柄（用于与 epoll 集成）
    // 返回值: CURLM 句柄
    CURLM* getMultiHandle();

    // 处理 curl multi 的 socket 回调
    // 将 AI HTTP 连接注册到主 Epoll 实例
    // 参数 epfd: Epoll 实例文件描述符
    void integrateWithEpoll(int epfd);

    // 处理 curl multi 事件（在 epoll_wait 检测到活跃后调用）
    void performAction(int sockfd, int ev_bitmask);

    // 检查并调用已完成的请求回调
    void processCompletedRequests();

    // 发送 AI 聊天请求
    // 参数 prompt: 用户输入
    // 参数 context: 对话上下文（JSON 格式的消息历史）
    // 参数 callback: 完成回调
    // 返回值: 请求ID（用于追踪）
    std::string chat(const std::string& prompt, const std::string& context,
                     AICallback callback);

    // 发送摘要请求
    // 参数 messages: 要摘要的消息列表（JSON 格式）
    // 参数 callback: 完成回调
    // 返回值: 请求ID
    std::string summarize(const std::string& messages, AICallback callback);

    // 发送情感分析请求
    // 参数 text: 要分析的文本
    // 参数 callback: 完成回调（返回 JSON 格式的情感分数）
    // 返回值: 请求ID
    std::string analyzeSentiment(const std::string& text, AICallback callback);

    // 获取 AI 服务健康状态
    // 返回值: 所有 Provider 的健康状态映射
    std::unordered_map<std::string, bool> getProviderHealth();

private:
    // 添加新的 curl easy 请求到 multi 句柄
    // 参数 url: API 端点
    // 参数 body: JSON 请求体
    // 参数 callback: 完成回调
    // 返回值: 请求ID
    std::string addRequest(const std::string& url, const std::string& body,
                           AICallback callback);

    // 处理单次故障转移
    // 参数 req: 失败的请求
    // 返回值: 是否还有备用 Provider
    bool tryFailover(AIRequest& req);

    // 获取下一个可用 Provider 的 URL
    // 返回值: Provider 的 API URL，无可用时返回空字符串
    std::string getNextProviderUrl();

    CURLM* multi_handle_;       // curl multi 句柄
    int    epfd_;               // Epoll 实例文件描述符（用于集成）
    int    current_provider_;   // 当前 Provider 索引（用于故障转移）

    // 请求ID → 请求上下文的映射
    std::unordered_map<std::string, AIRequest> pending_requests_;

    // curl easy 句柄 → 请求ID 的映射
    std::unordered_map<CURL*, std::string> curl_to_request_;

    // Provider URL 列表（按故障转移优先顺序排列）
    std::vector<std::string> provider_urls_;
};

#endif // AI_SERVICE_CLIENT_H
