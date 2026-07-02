// WebSocketCodec - RFC 6455 WebSocket 帧编解码器
// 负责 WebSocket 帧的解析（客户端→服务端）和生成（服务端→客户端）
// 支持分片消息重组、控制帧内联处理、掩码验证
// 参考：RFC 6455 Section 5 (Data Framing)

#ifndef WEBSOCKET_CODEC_H
#define WEBSOCKET_CODEC_H

#include <cstdint>
#include <string>
#include <vector>

// ============ WebSocket 常量（RFC 6455） ============

// 帧类型 Opcode（Section 5.2）
enum class WsOpcode : uint8_t {
    CONTINUATION = 0x0,  // 分片续帧
    TEXT         = 0x1,  // 文本帧（UTF-8）
    BINARY       = 0x2,  // 二进制帧
    CLOSE        = 0x8,  // 关闭连接
    PING         = 0x9,  // 心跳请求
    PONG         = 0xA,  // 心跳响应
};

// 关闭状态码（Section 7.4）
enum class WsCloseCode : uint16_t {
    NORMAL          = 1000,  // 正常关闭
    GOING_AWAY      = 1001,  // 端点离开
    PROTOCOL_ERROR  = 1002,  // 协议错误
    INVALID_DATA    = 1003,  // 无效数据类型（收到非 UTF-8 文本等）
    MESSAGE_TOO_BIG = 1009,  // 消息过大
};

// ============ 帧解析状态机 ============

// 帧解析阶段
enum class WsParseStage {
    HEADER_BASIC,         // 正在读取第 1-2 字节（FIN+Opcode+Mask+PayloadLen）
    HEADER_EXTENDED_LEN,  // 正在读取扩展长度（2 或 8 字节）
    HEADER_MASK_KEY,      // 正在读取掩码密钥（4 字节）
    PAYLOAD,              // 正在读取载荷数据
    COMPLETE,             // 一帧解析完成
};

// 帧解析中间状态（跨 TCP read() 调用保持）
struct WsFrameParseState {
    WsParseStage stage = WsParseStage::HEADER_BASIC;

    // 帧头字段
    bool    fin        = false;
    uint8_t opcode     = 0;
    bool    masked     = false;
    uint8_t mask_key[4] = {0, 0, 0, 0};
    uint64_t payload_length = 0;

    // 解析进度追踪
    uint64_t bytes_consumed = 0;   // 当前阶段已消费的字节数
    uint64_t bytes_needed   = 0;   // 当前阶段还需的字节数

    void reset() {
        stage = WsParseStage::HEADER_BASIC;
        fin = false;
        opcode = 0;
        masked = false;
        payload_length = 0;
        bytes_consumed = 0;
        bytes_needed = 0;
    }
};

// ============ 解析结果 ============

struct WsFrame {
    bool     fin       = false;
    uint8_t  opcode    = 0;
    bool     is_control = false;  // opcode >= 0x8
    std::vector<uint8_t> payload;  // 已去掩码的载荷
};

// ============ WebSocketCodec 类 ============

class WebSocketCodec {
public:
    // --- 配置 ---

    // 设置最大载荷大小（字节），超出返回 1009 错误
    static void setMaxPayloadSize(uint64_t max_size);
    static uint64_t maxPayloadSize();

    // --- 帧生成（服务端→客户端，不掩码） ---

    // 生成文本帧
    static std::vector<uint8_t> encodeText(const std::string& text);

    // 生成二进制帧
    static std::vector<uint8_t> encodeBinary(const std::vector<uint8_t>& data);

    // 生成关闭帧
    static std::vector<uint8_t> encodeClose(WsCloseCode code = WsCloseCode::NORMAL,
                                            const std::string& reason = "");

    // 生成 Pong 帧（响应客户端 Ping）
    static std::vector<uint8_t> encodePong(const std::vector<uint8_t>& ping_data);

    // 生成 Pong 帧（纯文本载荷）
    static std::vector<uint8_t> encodePong(const std::string& data);

    // 便捷方法：生成帧并转为 std::string（用于 sendToClient 兼容）
    static std::string encodeTextString(const std::string& text);
    static std::string encodeCloseString(WsCloseCode code = WsCloseCode::NORMAL,
                                         const std::string& reason = "");

    // --- WebSocket 握手 ---

    // 检查是否为 HTTP Upgrade 请求
    // 返回 true 如果包含 "Upgrade: websocket"
    static bool isUpgradeRequest(const std::string& http_request);

    // 生成握手响应（HTTP 101 Switching Protocols）
    static std::string generateHandshakeResponse(const std::string& http_request);

    // --- 帧解析（客户端→服务端，必须掩码） ---

    // 从原始字节缓冲区解析一帧
    // buffer: 包含原始字节的缓冲区（解析后消费的字节从前面移除）
    // frame:  输出参数，解析完成的帧
    // 返回值：
    //   true  - 完整帧已解析，frame 已填充
    //   false - 数据不完整，等待更多数据（frame 未修改）
    // 抛出：   帧格式错误时返回 PROTOCOL_ERROR 或 MESSAGE_TOO_BIG
    static bool tryParseFrame(std::vector<uint8_t>& buffer, WsFrame& frame,
                              WsFrameParseState& state);

    // --- 重组 ---

    // 检查是否需要重组（fin=false 或 opcode=CONTINUATION）
    static bool needsReassembly(const WsFrame& frame, bool in_fragmented);

    // 追加到重组缓冲区
    static void appendToReassembly(std::vector<uint8_t>& reassembly_buf,
                                   const WsFrame& frame);

    // 完成重组，返回完整消息载荷
    static std::string finalizeReassembly(std::vector<uint8_t>& reassembly_buf,
                                          uint8_t original_opcode);

private:
    static uint64_t max_payload_size_;
};

#endif // WEBSOCKET_CODEC_H
