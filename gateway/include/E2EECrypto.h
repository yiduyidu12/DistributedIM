// E2EECrypto - 端到端加密模块
// 基于 libsodium 实现简化版 Signal Protocol
// Ed25519 签名密钥对 + X25519 DH 密钥交换 + AES-256-GCM 每消息加密
// 编译条件：需定义 E2E_ENABLED 宏并链接 libsodium
// 服务端不可解密消息内容，仅路由密文

#ifndef E2EE_CRYPTO_H
#define E2EE_CRYPTO_H

#ifdef E2E_ENABLED

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <chrono>

// 密钥长度常量（bytes）
constexpr size_t ED25519_PUBLIC_KEY_BYTES  = 32;
constexpr size_t ED25519_SECRET_KEY_BYTES  = 64;
constexpr size_t X25519_PUBLIC_KEY_BYTES   = 32;
constexpr size_t X25519_SECRET_KEY_BYTES   = 32;
constexpr size_t AES256_GCM_KEY_BYTES      = 32;
constexpr size_t AES256_GCM_NONCE_BYTES    = 12;
constexpr size_t AES256_GCM_MAC_BYTES      = 16;

// 密钥对结构
// 每台设备独立维护自己的密钥对，不跨设备共享棘轮状态
struct E2EEKeyPair {
    std::vector<uint8_t> identity_pub;   // Ed25519 身份公钥（32 bytes）
    std::vector<uint8_t> identity_priv;  // Ed25519 身份私钥（64 bytes）
    std::vector<uint8_t> signed_prekey_pub;  // 已签名预密钥公钥
    std::vector<uint8_t> signed_prekey_priv; // 已签名预密钥私钥
    std::chrono::system_clock::time_point created_at;  // 密钥创建时间（用于轮换判断）

    // 序列化为 JSON（用于密钥分发）
    std::string publicKeysToJson() const;

    // 从 JSON 反序列化（仅公钥部分）
    static E2EEKeyPair publicKeysFromJson(const std::string& json);

    // 获取密钥已使用时长（秒）
    // 返回值: 从创建到现在的秒数
    int ageSeconds() const;
};

// 会话密钥状态
// 每次 DH 密钥交换后派生，包含发送链和接收链的独立棘轮
struct SessionState {
    std::vector<uint8_t> root_key;        // 根密钥（32 bytes）
    std::vector<uint8_t> send_chain_key;  // 发送链密钥（32 bytes）
    std::vector<uint8_t> recv_chain_key;  // 接收链密钥（32 bytes）
    uint64_t send_count;                  // 已发送消息计数（防重放）
    uint64_t recv_count;                  // 已接收消息计数
    std::vector<uint8_t> last_nonce;      // 上次使用的 nonce
};

// E2EE 加密管理器
// 管理所有用户/设备的密钥对、会话状态和加密操作
class E2EEManager {
public:
    // 获取单例实例
    static E2EEManager& instance();

    // 初始化 libsodium 库
    // 返回值: 初始化成功返回true
    bool init();

    // 检查硬件 AES 加速是否可用
    // 返回值: 支持 AES-NI 等硬件加速返回true
    bool hasHardwareAES() const;

    // ============ 密钥生成 ============

    // 为当前网关用户/设备生成新的身份密钥对
    // 参数 device_id: 设备唯一标识
    // 返回值: 生成的密钥对（包含公私钥）
    E2EEKeyPair generateIdentityKeys(const std::string& device_id);

    // 从文件加载密钥对
    // 参数 device_id: 设备ID
    // 参数 kp: 输出密钥对
    // 返回值: 加载成功返回true
    bool loadKeyPair(const std::string& device_id, E2EEKeyPair& kp);

    // 保存密钥对到文件
    // 参数 device_id: 设备ID
    // 参数 kp: 要保存的密钥对
    // 返回值: 保存成功返回true
    bool saveKeyPair(const std::string& device_id, const E2EEKeyPair& kp);

    // ============ 密钥交换（X3DH 简化版） ============

    // 执行 DH 密钥交换，派生共享密钥
    // 参数 our_keys: 本地密钥对
    // 参数 their_identity_pub: 对方身份公钥
    // 参数 their_prekey_pub: 对方预密钥公钥
    // 参数 session: 输出会话状态
    // 返回值: 交换成功返回true
    bool performKeyExchange(const E2EEKeyPair& our_keys,
                            const std::vector<uint8_t>& their_identity_pub,
                            const std::vector<uint8_t>& their_prekey_pub,
                            SessionState& session);

    // ============ 消息加密/解密（Double Ratchet 简化版） ============

    // 加密一条消息
    // 参数 session: 会话状态（会更新发送棘轮）
    // 参数 plaintext: 明文消息
    // 参数 ciphertext: 输出密文（包含 nonce + MAC）
    // 返回值: 加密成功返回true
    bool encryptMessage(SessionState& session,
                        const std::string& plaintext,
                        std::vector<uint8_t>& ciphertext);

    // 解密一条消息
    // 参数 session: 会话状态（会更新接收棘轮）
    // 参数 ciphertext: 密文数据（nonce + cipher + MAC）
    // 参数 plaintext: 输出明文
    // 返回值: 解密成功返回true，MAC 验证失败返回false
    bool decryptMessage(SessionState& session,
                        const std::vector<uint8_t>& ciphertext,
                        std::string& plaintext);

    // ============ 会话管理 ============

    // 获取或创建与指定用户的会话
    // 参数 peer_username: 对方用户名
    // 返回值: 会话状态指针，不存在返回nullptr
    SessionState* getSession(const std::string& peer_username);

    // 存储会话状态
    // 参数 peer_username: 对方用户名
    // 参数 session: 会话状态
    void storeSession(const std::string& peer_username, const SessionState& session);

    // 删除会话（用户登出或密钥重置时调用）
    // 参数 peer_username: 对方用户名
    void deleteSession(const std::string& peer_username);

    // ============ 密钥分发协议消息处理 ============

    // 生成密钥分发请求（请求对方公钥）
    // 参数 from_user: 请求方用户名
    // 返回值: JSON 格式的请求消息
    std::string createKeyRequest(const std::string& from_user);

    // 生成密钥分发响应（发送己方公钥）
    // 参数 device_id: 本设备ID
    // 参数 kp: 本地密钥对
    // 返回值: JSON 格式的响应消息
    std::string createKeyResponse(const std::string& device_id,
                                  const E2EEKeyPair& kp);

    // 处理收到的密钥分发响应并建立会话
    // 参数 from_user: 密钥来源用户
    // 参数 json_response: 密钥响应JSON
    // 返回值: 会话建立成功返回true
    bool handleKeyResponse(const std::string& from_user,
                           const std::string& json_response);

    // ============ 密钥轮换 ============

    // 检查密钥是否需要轮换（超过轮换周期）
    // 参数 device_id: 设备ID
    // 返回值: 需要轮换返回true
    bool needsKeyRotation(const std::string& device_id);

    // 轮换指定设备的密钥对
    // 生成新密钥对，更新内存和持久化存储
    // 参数 device_id: 设备ID
    // 返回值: 轮换成功返回true，生成的新密钥对
    bool rotateKeyPair(const std::string& device_id, E2EEKeyPair& new_kp);

    // 设置密钥轮换周期（秒）
    // 参数 seconds: 新的轮换周期
    void setRotationPeriod(int seconds);

private:
    // 构造函数（私有，单例模式）
    E2EEManager();
    ~E2EEManager();

    // 禁止拷贝
    E2EEManager(const E2EEManager&) = delete;
    E2EEManager& operator=(const E2EEManager&) = delete;

    // HKDF 密钥派生（链密钥递进）
    // 参数 input_key: 输入密钥材料
    // 参数 info: 上下文标签
    // 参数 output_key: 输出密钥（32 bytes）
    void hkdfDerive(const std::vector<uint8_t>& input_key,
                    const std::string& info,
                    std::vector<uint8_t>& output_key);

    // 棘轮递进：从链密钥派生消息密钥和下一链密钥
    // 参数 chain_key: 当前链密钥（输入且被更新）
    // 参数 message_key: 输出消息密钥（32 bytes）
    void ratchetStep(std::vector<uint8_t>& chain_key,
                     std::vector<uint8_t>& message_key);

    bool initialized_;  // libsodium 初始化标志
    std::string key_dir_;  // 密钥存储目录

    // 密钥轮换周期（秒），默认7天（604800秒）
    int rotation_period_;

    // 设备ID → 密钥对映射
    std::unordered_map<std::string, E2EEKeyPair> key_pairs_;
    std::mutex key_mutex_;

    // 对方用户名 → 会话状态映射
    std::unordered_map<std::string, SessionState> sessions_;
    std::mutex session_mutex_;
};

#endif // E2E_ENABLED

#endif // E2EE_CRYPTO_H
