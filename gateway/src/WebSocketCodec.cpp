// WebSocketCodec - RFC 6455 WebSocket 帧编解码器
// 负责 WebSocket 帧的解析（客户端→服务端）和生成（服务端→客户端）
// 支持分片消息重组、控制帧内联处理、掩码验证
// 参考：RFC 6455 Section 5 (Data Framing)

#include "WebSocketCodec.h"
#include "Logger.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <openssl/sha.h>
#include <iomanip>

// 静态成员初始化
uint64_t WebSocketCodec::max_payload_size_ = 65536;

// 设置最大载荷大小
// 参数 max_size: 最大字节数，超出返回 1009 错误
void WebSocketCodec::setMaxPayloadSize(uint64_t max_size) { max_payload_size_ = max_size; }

// 获取最大载荷大小
// 返回值: 当前配置的最大载荷字节数
uint64_t WebSocketCodec::maxPayloadSize() { return max_payload_size_; }

// ============ 帧头构建（内部辅助） ============

// 构建 WebSocket 帧头字节
// 参数 frame: 输出帧数据（追加模式）
// 参数 fin: 是否为最后一帧
// 参数 opcode: 帧操作码
// 参数 masked: 是否掩码（客户端→服务端必须为true）
// 参数 payload_len: 载荷长度
// 参数 mask_key: 掩码密钥（4字节），可为nullptr
static void appendFrameHeader(std::vector<uint8_t>& frame, bool fin, uint8_t opcode,
                              bool masked, uint64_t payload_len, const uint8_t* mask_key) {
    // 第一个字节：FIN(1bit) + RSV(3bit) + Opcode(4bit)
    uint8_t b0 = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    frame.push_back(b0);

    // 第二个字节：MASK(1bit) + PayloadLen(7bit)
    uint8_t b1 = masked ? 0x80 : 0x00;
    if (payload_len <= 125) {
        b1 |= static_cast<uint8_t>(payload_len);
        frame.push_back(b1);
    } else if (payload_len <= 65535) {
        // 扩展长度 16 位
        b1 |= 126;
        frame.push_back(b1);
        frame.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    } else {
        // 扩展长度 64 位
        b1 |= 127;
        frame.push_back(b1);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF));
    }

    // 掩码密钥（仅客户端→服务端帧需要）
    if (masked && mask_key) {
        frame.insert(frame.end(), mask_key, mask_key + 4);
    }
}

// 应用/解除 XOR 掩码（对称操作）
// 参数 data: 要进行掩码操作的数据
// 参数 mask_key: 4 字节掩码密钥
static void applyMask(std::vector<uint8_t>& data, const uint8_t mask_key[4]) {
    for (size_t i = 0; i < data.size(); ++i)
        data[i] ^= mask_key[i % 4];
}

// ============ 帧生成（服务端→客户端，不掩码） ============

// 生成文本帧
// 参数 text: UTF-8 编码的文本内容
// 返回值: 完整的 WebSocket 文本帧（二进制）
std::vector<uint8_t> WebSocketCodec::encodeText(const std::string& text) {
    std::vector<uint8_t> frame;
    appendFrameHeader(frame, true, static_cast<uint8_t>(WsOpcode::TEXT), false, text.size(), nullptr);
    frame.insert(frame.end(), text.begin(), text.end());
    return frame;
}

// 生成二进制帧
// 参数 data: 二进制数据
// 返回值: 完整的 WebSocket 二进制帧
std::vector<uint8_t> WebSocketCodec::encodeBinary(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame;
    appendFrameHeader(frame, true, static_cast<uint8_t>(WsOpcode::BINARY), false, data.size(), nullptr);
    frame.insert(frame.end(), data.begin(), data.end());
    return frame;
}

// 生成关闭帧
// 参数 code: 关闭状态码（RFC 6455 Section 7.4）
// 参数 reason: 关闭原因描述（可选）
// 返回值: 完整的 WebSocket 关闭帧
std::vector<uint8_t> WebSocketCodec::encodeClose(WsCloseCode code, const std::string& reason) {
    std::vector<uint8_t> payload;
    uint16_t code_val = static_cast<uint16_t>(code);
    payload.push_back(static_cast<uint8_t>((code_val >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code_val & 0xFF));
    payload.insert(payload.end(), reason.begin(), reason.end());

    std::vector<uint8_t> frame;
    appendFrameHeader(frame, true, static_cast<uint8_t>(WsOpcode::CLOSE), false, payload.size(), nullptr);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// 生成 Pong 帧（响应客户端 Ping）
// 参数 data: 客户端 Ping 帧的载荷数据（原样回传）
// 返回值: 完整的 Pong 帧
std::vector<uint8_t> WebSocketCodec::encodePong(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame;
    appendFrameHeader(frame, true, static_cast<uint8_t>(WsOpcode::PONG), false, data.size(), nullptr);
    frame.insert(frame.end(), data.begin(), data.end());
    return frame;
}

// 生成 Pong 帧（文本载荷版本）
// 参数 data: 文本内容
// 返回值: 完整的 Pong 帧
std::vector<uint8_t> WebSocketCodec::encodePong(const std::string& data) {
    std::vector<uint8_t> frame;
    appendFrameHeader(frame, true, static_cast<uint8_t>(WsOpcode::PONG), false, data.size(), nullptr);
    frame.insert(frame.end(), data.begin(), data.end());
    return frame;
}

// 便捷方法：生成文本帧并转为 std::string
// 用于与现有 sendToClient 接口兼容
// 参数 text: UTF-8 编码的文本内容
// 返回值: 完整帧的字符串表示
std::string WebSocketCodec::encodeTextString(const std::string& text) {
    auto frame = encodeText(text);
    return std::string(frame.begin(), frame.end());
}

// 便捷方法：生成关闭帧并转为 std::string
// 参数 code: 关闭状态码
// 参数 reason: 关闭原因
// 返回值: 完整关闭帧的字符串表示
std::string WebSocketCodec::encodeCloseString(WsCloseCode code, const std::string& reason) {
    auto frame = encodeClose(code, reason);
    return std::string(frame.begin(), frame.end());
}

// ============ WebSocket 握手 ============

// Base64 编码（用于 Sec-WebSocket-Accept 计算）
// 参数 data: 原始字节数据
// 参数 len: 数据长度
// 返回值: Base64 编码字符串
static std::string base64Encode(const unsigned char* data, size_t len) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        result += chars[(n >> 18) & 0x3F];
        result += chars[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? chars[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? chars[n & 0x3F] : '=';
    }
    return result;
}

// 检查是否为 WebSocket Upgrade 请求
// 参数 http_request: HTTP 请求头文本
// 返回值: 包含 "Upgrade: websocket" 头返回 true
bool WebSocketCodec::isUpgradeRequest(const std::string& http_request) {
    return http_request.find("Upgrade: websocket") != std::string::npos ||
           http_request.find("upgrade: websocket") != std::string::npos;
}

// 生成 WebSocket 握手响应
// 根据客户端 Sec-WebSocket-Key 计算 Accept 值，构建 HTTP 101 响应
// 参数 http_request: 客户端 HTTP 升级请求
// 返回值: HTTP 101 Switching Protocols 响应文本
std::string WebSocketCodec::generateHandshakeResponse(const std::string& http_request) {
    // 从请求头中提取 Sec-WebSocket-Key
    std::string key;
    std::istringstream stream(http_request);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("Sec-WebSocket-Key:") != std::string::npos ||
            line.find("Sec-WebSocket-Key: ") != std::string::npos) {
            size_t pos = line.find(": ");
            if (pos != std::string::npos) {
                key = line.substr(pos + 2);
                // 去除尾部换行和回车
                while (!key.empty() && (key.back() == '\r' || key.back() == '\n'))
                    key.pop_back();
            }
        }
    }

    // 拼接 Magic GUID 并计算 SHA1
    static const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic;

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.size(), hash);

    // Base64 编码 SHA1 结果得到 Accept 值
    std::string accept = base64Encode(hash, SHA_DIGEST_LENGTH);

    // 构建 HTTP 101 响应
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
             << "Upgrade: websocket\r\n"
             << "Connection: Upgrade\r\n"
             << "Sec-WebSocket-Accept: " << accept << "\r\n"
             << "\r\n";
    return response.str();
}

// ============ 帧解析（客户端→服务端，必须掩码） ============

// 从原始字节缓冲区解析一帧
// 使用状态机模式支持跨 TCP read() 调用的分步解析
// 解析流程：HEADER_BASIC → HEADER_EXTENDED_LEN → HEADER_MASK_KEY → PAYLOAD → COMPLETE
// 参数 buffer: 包含原始字节的缓冲区（解析后消费的字节从前面移除）
// 参数 frame: 输出参数，解析完成的帧
// 参数 state: 跨调用保持的解析中间状态
// 返回值: true=完整帧已解析, false=等待更多数据
bool WebSocketCodec::tryParseFrame(std::vector<uint8_t>& buffer, WsFrame& frame,
                                   WsFrameParseState& state) {
    // 防止空缓冲区导致死循环
    if (buffer.empty()) return false;

    // 阶段1：解析基本帧头（第 1-2 字节），包含 FIN + Opcode + MASK + PayloadLen
    if (state.stage == WsParseStage::HEADER_BASIC) {
        if (buffer.size() < 2) return false;

        uint8_t b0 = buffer[0];
        uint8_t b1 = buffer[1];

        state.fin    = (b0 & 0x80) != 0;
        state.opcode = b0 & 0x0F;
        state.masked = (b1 & 0x80) != 0;

        // 客户端到服务端帧必须掩码（RFC 6455 Section 5.1）
        if (!state.masked) {
            Logger::warn("[WS] 帧未掩码，协议错误");
            return false;
        }

        state.payload_length = b1 & 0x7F;

        if (state.payload_length == 126) {
            // 扩展长度 16 位
            state.stage = WsParseStage::HEADER_EXTENDED_LEN;
            state.bytes_needed = 2;
            state.bytes_consumed = 0;
            buffer.erase(buffer.begin(), buffer.begin() + 2);
        } else if (state.payload_length == 127) {
            // 扩展长度 64 位
            state.stage = WsParseStage::HEADER_EXTENDED_LEN;
            state.bytes_needed = 8;
            state.bytes_consumed = 0;
            buffer.erase(buffer.begin(), buffer.begin() + 2);
        } else {
            // 7 位内联长度，直接进入掩码密钥阶段
            state.stage = WsParseStage::HEADER_MASK_KEY;
            state.bytes_needed = 4;
            state.bytes_consumed = 0;
            buffer.erase(buffer.begin(), buffer.begin() + 2);
        }
        return false;
    }

    // 阶段2：解析扩展长度（2 或 8 字节）
    if (state.stage == WsParseStage::HEADER_EXTENDED_LEN) {
        size_t avail = std::min(buffer.size(), static_cast<size_t>(state.bytes_needed - state.bytes_consumed));
        if (avail == 0) return false;

        // 从缓冲区读取扩展长度字节（大端序）
        for (size_t i = 0; i < avail; ++i)
            state.payload_length = (state.payload_length << 8) | buffer[i];

        state.bytes_consumed += avail;
        buffer.erase(buffer.begin(), buffer.begin() + avail);

        if (state.bytes_consumed >= state.bytes_needed) {
            // 扩展长度解析完成，进入掩码密钥阶段
            state.stage = WsParseStage::HEADER_MASK_KEY;
            state.bytes_needed = 4;
            state.bytes_consumed = 0;
        } else {
            return false;
        }
    }

    // 阶段3：解析掩码密钥（4 字节）
    if (state.stage == WsParseStage::HEADER_MASK_KEY) {
        size_t avail = std::min(buffer.size(), static_cast<size_t>(4 - state.bytes_consumed));
        if (avail == 0) return false;

        for (size_t i = 0; i < avail; ++i)
            state.mask_key[state.bytes_consumed + i] = buffer[i];
        state.bytes_consumed += avail;
        buffer.erase(buffer.begin(), buffer.begin() + avail);

        if (state.bytes_consumed >= 4) {
            // 检查载荷大小限制
            if (state.payload_length > max_payload_size_) {
                Logger::warn("[WS] 载荷超限: {} > {}，返回 1009 错误",
                            state.payload_length, max_payload_size_);
                return false;
            }
            // 掩码密钥就绪，进入载荷读取阶段
            state.stage = WsParseStage::PAYLOAD;
            state.bytes_needed = state.payload_length;
            state.bytes_consumed = 0;
        } else {
            return false;
        }
    }

    // 阶段4：读取载荷数据并解除掩码
    if (state.stage == WsParseStage::PAYLOAD) {
        size_t remaining = state.bytes_needed - state.bytes_consumed;
        size_t avail = std::min(buffer.size(), remaining);
        if (avail == 0) return false;

        // 积累载荷数据到帧对象
        frame.payload.insert(frame.payload.end(), buffer.begin(), buffer.begin() + avail);
        state.bytes_consumed += avail;
        buffer.erase(buffer.begin(), buffer.begin() + avail);

        // 载荷读取完成，解除掩码并重置状态机
        if (state.bytes_consumed >= state.bytes_needed) {
            applyMask(frame.payload, state.mask_key);

            frame.fin    = state.fin;
            frame.opcode = state.opcode;
            frame.is_control = (state.opcode & 0x08) != 0;

            state.reset();
            return true;
        }
    }

    return false;
}

// 检查是否需要分片重组
// 参数 frame: 当前帧
// 参数 in_fragmented: 是否已在重组模式中
// 返回值: 需要继续重组返回 true
bool WebSocketCodec::needsReassembly(const WsFrame& frame, bool in_fragmented) {
    if (in_fragmented) return true;
    if (!frame.fin) return true;
    if (frame.opcode == static_cast<uint8_t>(WsOpcode::CONTINUATION)) return true;
    return false;
}

// 追加帧载荷到重组缓冲区
// 参数 buf: 重组缓冲区
// 参数 frame: 当前分片帧
void WebSocketCodec::appendToReassembly(std::vector<uint8_t>& buf, const WsFrame& frame) {
    buf.insert(buf.end(), frame.payload.begin(), frame.payload.end());
}

// 完成重组并返回完整消息
// 参数 buf: 重组缓冲区（调用后会被清空）
// 参数 original_opcode: 首帧的操作码（预留字段）
// 返回值: 重组后的完整消息字符串
std::string WebSocketCodec::finalizeReassembly(std::vector<uint8_t>& buf, uint8_t) {
    std::string result(buf.begin(), buf.end());
    buf.clear();
    return result;
}
