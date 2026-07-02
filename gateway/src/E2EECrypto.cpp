// E2EECrypto - 端到端加密模块实现
// 基于 libsodium 实现 Ed25519 密钥对 + X25519 DH 密钥交换 + AES-256-GCM 加密
// 编译条件：需定义 E2E_ENABLED 宏并链接 libsodium
// 单例模式管理所有用户的密钥对和会话棘轮状态

#ifdef E2E_ENABLED

#include "E2EECrypto.h"
#include "Logger.h"
#include <sodium.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstring>
#include <sys/stat.h>
#include <cerrno>

// ============ E2EEKeyPair 实现 ============

// 将公钥部分序列化为 JSON（发送给对方进行密钥交换）
// 返回值: JSON 字符串，包含 identity_pub 和 signed_prekey_pub 的 Base64 编码
std::string E2EEKeyPair::publicKeysToJson() const {
    nlohmann::json j;
    // Base64 编码公钥便于 JSON 传输
    auto toBase64 = [](const std::vector<uint8_t>& data) -> std::string {
        size_t b64_max = sodium_base64_encoded_len(data.size(), sodium_base64_VARIANT_ORIGINAL);
        std::string result(b64_max, '\0');
        sodium_bin2base64(result.data(), result.size(),
                          data.data(), data.size(),
                          sodium_base64_VARIANT_ORIGINAL);
        // 去除尾部 \0
        size_t len = strlen(result.c_str());
        result.resize(len);
        return result;
    };

    j["identity_pub"] = toBase64(identity_pub);
    j["signed_prekey_pub"] = toBase64(signed_prekey_pub);
    return j.dump();
}

// 从 JSON 反序列化公钥（接收对方公钥时调用）
// 参数 json: 包含 Base64 编码公钥的 JSON 字符串
// 返回值: 仅包含公钥字段的 E2EEKeyPair 对象
E2EEKeyPair E2EEKeyPair::publicKeysFromJson(const std::string& json) {
    auto j = nlohmann::json::parse(json);
    auto fromBase64 = [](const std::string& b64) -> std::vector<uint8_t> {
        size_t bin_max = b64.size() * 3 / 4 + 1;
        std::vector<uint8_t> result(bin_max);
        size_t bin_len = 0;
        sodium_base642bin(result.data(), result.size(),
                          b64.data(), b64.size(),
                          nullptr, &bin_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL);
        result.resize(bin_len);
        return result;
    };

    E2EEKeyPair kp;
    kp.identity_pub = fromBase64(j.value("identity_pub", ""));
    kp.signed_prekey_pub = fromBase64(j.value("signed_prekey_pub", ""));
    return kp;
}

// 获取密钥已使用时长（秒）
// 返回值: 从创建到现在的秒数
int E2EEKeyPair::ageSeconds() const {
    auto now = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - created_at);
    return static_cast<int>(duration.count());
}

// ============ E2EEManager 实现 ============

// 获取单例实例
// 返回值: 全局唯一的 E2EEManager 引用
E2EEManager& E2EEManager::instance() {
    static E2EEManager inst;
    return inst;
}

// 构造函数（私有）
E2EEManager::E2EEManager()
    : initialized_(false)
    , key_dir_("./e2ee_keys")
    , rotation_period_(604800) {  // 默认7天轮换周期
}

// 析构函数
E2EEManager::~E2EEManager() {
}

// 初始化 libsodium 库
// 必须在其他任何操作前调用
// 返回值: 初始化成功返回true
bool E2EEManager::init() {
    if (sodium_init() < 0) {
        Logger::error("[E2EE] libsodium 初始化失败");
        return false;
    }

    // 确保密钥存储目录存在
    struct stat st;
    if (stat(key_dir_.c_str(), &st) != 0) {
        mkdir(key_dir_.c_str(), 0700);
    }

    initialized_ = true;
    Logger::info("[E2EE] E2EE 加密模块已初始化, 硬件AES={}",
                 hasHardwareAES() ? "支持" : "不支持");
    return true;
}

// 检查硬件 AES 加速是否可用
// 返回值: 支持 AES-NI 等硬件加速返回true
bool E2EEManager::hasHardwareAES() const {
    return crypto_aead_aes256gcm_is_available() != 0;
}

// ============ 密钥生成 ============

// 为指定设备生成 Ed25519 身份密钥对 + X25519 预密钥
// 密钥对存储在内存中，并持久化到文件
// 参数 device_id: 设备唯一标识
// 返回值: 新生成的密钥对
E2EEKeyPair E2EEManager::generateIdentityKeys(const std::string& device_id) {
    E2EEKeyPair kp;

    // 生成 Ed25519 身份密钥对
    kp.identity_pub.resize(crypto_sign_PUBLICKEYBYTES);
    kp.identity_priv.resize(crypto_sign_SECRETKEYBYTES);
    crypto_sign_keypair(kp.identity_pub.data(), kp.identity_priv.data());

    // 生成 X25519 预密钥对（用于 DH 密钥交换）
    kp.signed_prekey_pub.resize(crypto_scalarmult_BYTES);
    kp.signed_prekey_priv.resize(crypto_scalarmult_SCALARBYTES);
    crypto_kx_keypair(kp.signed_prekey_pub.data(), kp.signed_prekey_priv.data());

    // 设置密钥创建时间（用于轮换判断）
    kp.created_at = std::chrono::system_clock::now();

    // 存储到内存
    {
        std::lock_guard<std::mutex> lock(key_mutex_);
        key_pairs_[device_id] = kp;
    }

    // 持久化到文件
    saveKeyPair(device_id, kp);

    Logger::info("[E2EE] 生成身份密钥对: device={}", device_id);
    return kp;
}

// 从文件加载密钥对
// 参数 device_id: 设备ID
// 参数 kp: 输出密钥对
// 返回值: 加载成功返回true
bool E2EEManager::loadKeyPair(const std::string& device_id, E2EEKeyPair& kp) {
    // 先检查内存缓存
    {
        std::lock_guard<std::mutex> lock(key_mutex_);
        auto it = key_pairs_.find(device_id);
        if (it != key_pairs_.end()) {
            kp = it->second;
            return true;
        }
    }

    // 从文件读取
    std::string path = key_dir_ + "/" + device_id + ".json";
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::warn("[E2EE] 密钥文件不存在: {}", path);
        return false;
    }

    std::string json((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
    auto j = nlohmann::json::parse(json);

    auto fromBase64 = [](const std::string& b64) -> std::vector<uint8_t> {
        size_t bin_max = b64.size() * 3 / 4 + 1;
        std::vector<uint8_t> result(bin_max);
        size_t bin_len = 0;
        sodium_base642bin(result.data(), result.size(),
                          b64.data(), b64.size(),
                          nullptr, &bin_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL);
        result.resize(bin_len);
        return result;
    };

    kp.identity_pub = fromBase64(j.value("identity_pub", ""));
    kp.identity_priv = fromBase64(j.value("identity_priv", ""));
    kp.signed_prekey_pub = fromBase64(j.value("signed_prekey_pub", ""));
    kp.signed_prekey_priv = fromBase64(j.value("signed_prekey_priv", ""));
    // 加载密钥创建时间（用于轮换判断）
    time_t created_time = j.value("created_at", 0LL);
    kp.created_at = std::chrono::system_clock::from_time_t(created_time);

    // 缓存到内存
    {
        std::lock_guard<std::mutex> lock(key_mutex_);
        key_pairs_[device_id] = kp;
    }

    Logger::info("[E2EE] 加载密钥对: device={}", device_id);
    return true;
}

// 保存密钥对到文件
// 参数 device_id: 设备ID
// 参数 kp: 要保存的密钥对
// 返回值: 保存成功返回true
bool E2EEManager::saveKeyPair(const std::string& device_id, const E2EEKeyPair& kp) {
    auto toBase64 = [](const std::vector<uint8_t>& data) -> std::string {
        size_t b64_max = sodium_base64_encoded_len(data.size(), sodium_base64_VARIANT_ORIGINAL);
        std::string result(b64_max, '\0');
        sodium_bin2base64(result.data(), result.size(),
                          data.data(), data.size(),
                          sodium_base64_VARIANT_ORIGINAL);
        size_t len = strlen(result.c_str());
        result.resize(len);
        return result;
    };

    nlohmann::json j;
    j["identity_pub"] = toBase64(kp.identity_pub);
    j["identity_priv"] = toBase64(kp.identity_priv);
    j["signed_prekey_pub"] = toBase64(kp.signed_prekey_pub);
    j["signed_prekey_priv"] = toBase64(kp.signed_prekey_priv);
    // 保存密钥创建时间（Unix时间戳，用于轮换判断）
    j["created_at"] = std::chrono::system_clock::to_time_t(kp.created_at);

    std::string path = key_dir_ + "/" + device_id + ".json";
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        Logger::error("[E2EE] 无法写入密钥文件: {}", path);
        return false;
    }
    file << j.dump();

    return true;
}

// ============ 密钥交换（X3DH 简化版） ============

// 执行 DH 密钥交换，派生共享密钥和会话棘轮
// 参数 our_keys: 本地密钥对（含身份密钥和预密钥）
// 参数 their_identity_pub: 对方 Ed25519 身份公钥
// 参数 their_prekey_pub: 对方 X25519 预密钥公钥
// 参数 session: 输出会话状态（含根密钥和收发链密钥）
// 返回值: 交换成功返回true
bool E2EEManager::performKeyExchange(const E2EEKeyPair& our_keys,
                                     const std::vector<uint8_t>& their_identity_pub,
                                     const std::vector<uint8_t>& their_prekey_pub,
                                     SessionState& session) {
    // Ed25519 身份密钥转 X25519
    std::vector<uint8_t> our_x25519_pub(crypto_scalarmult_BYTES);
    std::vector<uint8_t> our_x25519_priv(crypto_scalarmult_SCALARBYTES);

    if (crypto_sign_ed25519_pk_to_curve25519(our_x25519_pub.data(),
                                              our_keys.identity_pub.data()) != 0) {
        Logger::error("[E2EE] Ed25519→X25519 公钥转换失败");
        return false;
    }
    if (crypto_sign_ed25519_sk_to_curve25519(our_x25519_priv.data(),
                                              our_keys.identity_priv.data()) != 0) {
        Logger::error("[E2EE] Ed25519→X25519 私钥转换失败");
        return false;
    }

    // 对方身份公钥转 X25519
    std::vector<uint8_t> their_x25519_pub(crypto_scalarmult_BYTES);
    if (crypto_sign_ed25519_pk_to_curve25519(their_x25519_pub.data(),
                                              their_identity_pub.data()) != 0) {
        Logger::error("[E2EE] 对方 Ed25519→X25519 公钥转换失败");
        return false;
    }

    // X25519 DH 交换 1: 身份密钥 × 预密钥 → shared1
    std::vector<uint8_t> shared1(crypto_scalarmult_BYTES);
    if (crypto_scalarmult(shared1.data(), our_x25519_priv.data(),
                          their_prekey_pub.data()) != 0) {
        Logger::error("[E2EE] DH 交换1 失败");
        return false;
    }

    // X25519 DH 交换 2: 预密钥 × 身份密钥 → shared2
    std::vector<uint8_t> shared2(crypto_scalarmult_BYTES);
    if (crypto_scalarmult(shared2.data(), our_keys.signed_prekey_priv.data(),
                          their_x25519_pub.data()) != 0) {
        Logger::error("[E2EE] DH 交换2 失败");
        return false;
    }

    // 组合: root_key = HKDF(shared1 || shared2)
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), shared1.begin(), shared1.end());
    combined.insert(combined.end(), shared2.begin(), shared2.end());

    session.root_key.resize(AES256_GCM_KEY_BYTES);
    hkdfDerive(combined, "root_key", session.root_key);

    // 初始化发送链和接收链
    session.send_chain_key = session.root_key;
    session.recv_chain_key = session.root_key;
    session.send_count = 0;
    session.recv_count = 0;
    session.last_nonce.resize(AES256_GCM_NONCE_BYTES);

    // 清零临时密钥材料
    sodium_memzero(shared1.data(), shared1.size());
    sodium_memzero(shared2.data(), shared2.size());
    sodium_memzero(combined.data(), combined.size());
    sodium_memzero(our_x25519_priv.data(), our_x25519_priv.size());

    Logger::info("[E2EE] 密钥交换完成");
    return true;
}

// ============ 消息加密/解密 ============

// 加密一条消息
// 使用发送链密钥进行 AES-256-GCM 加密，每次加密后棘轮递进
// 参数 session: 会话状态（会更新发送棘轮）
// 参数 plaintext: 明文消息
// 参数 ciphertext: 输出密文（nonce + ciphertext + MAC）
// 返回值: 加密成功返回true
bool E2EEManager::encryptMessage(SessionState& session,
                                 const std::string& plaintext,
                                 std::vector<uint8_t>& ciphertext) {
    // 棘轮递进，获取本次消息密钥
    std::vector<uint8_t> message_key(AES256_GCM_KEY_BYTES);
    ratchetStep(session.send_chain_key, message_key);

    // 生成随机 nonce
    std::vector<uint8_t> nonce(AES256_GCM_NONCE_BYTES);
    randombytes_buf(nonce.data(), nonce.size());

    // 准备明文缓冲区
    std::vector<uint8_t> plain_bytes(plaintext.begin(), plaintext.end());

    // AES-256-GCM 加密
    ciphertext.resize(nonce.size() + plain_bytes.size() + crypto_aead_aes256gcm_ABYTES);

    // 写入 nonce（前缀）
    std::memcpy(ciphertext.data(), nonce.data(), nonce.size());

    unsigned long long ct_len = 0;
    int ret = crypto_aead_aes256gcm_encrypt(
        ciphertext.data() + nonce.size(), &ct_len,
        plain_bytes.data(), plain_bytes.size(),
        nullptr, 0,           // additional data
        nullptr,              // secret nonce (unused)
        nonce.data(),
        message_key.data());

    session.send_count++;
    session.last_nonce = nonce;

    // 清零消息密钥
    sodium_memzero(message_key.data(), message_key.size());

    return ret == 0;
}

// 解密一条消息
// 从密文中提取 nonce，使用接收链密钥进行 AES-256-GCM 解密
// 参数 session: 会话状态（会更新接收棘轮）
// 参数 ciphertext: 密文数据（nonce + ciphertext + MAC）
// 参数 plaintext: 输出明文
// 返回值: 解密成功返回true，MAC 验证失败返回false
bool E2EEManager::decryptMessage(SessionState& session,
                                 const std::vector<uint8_t>& ciphertext,
                                 std::string& plaintext) {
    // 提取 nonce
    if (ciphertext.size() < AES256_GCM_NONCE_BYTES + crypto_aead_aes256gcm_ABYTES) {
        Logger::warn("[E2EE] 密文太短, size={}", ciphertext.size());
        return false;
    }

    std::vector<uint8_t> nonce(ciphertext.begin(),
                               ciphertext.begin() + AES256_GCM_NONCE_BYTES);

    size_t ct_start = AES256_GCM_NONCE_BYTES;
    size_t ct_size = ciphertext.size() - ct_start;

    // 棘轮递进，获取本次消息密钥
    std::vector<uint8_t> message_key(AES256_GCM_KEY_BYTES);
    ratchetStep(session.recv_chain_key, message_key);

    // AES-256-GCM 解密
    std::vector<uint8_t> plain_bytes(ct_size);
    unsigned long long pt_len = 0;

    int ret = crypto_aead_aes256gcm_decrypt(
        plain_bytes.data(), &pt_len,
        nullptr,              // secret nonce (unused)
        ciphertext.data() + ct_start, ct_size,
        nullptr, 0,           // additional data
        nonce.data(),
        message_key.data());

    // 清零消息密钥
    sodium_memzero(message_key.data(), message_key.size());

    if (ret != 0) {
        Logger::warn("[E2EE] MAC 验证失败, 消息可能被篡改");
        return false;
    }

    session.recv_count++;
    plain_bytes.resize(pt_len);
    plaintext.assign(plain_bytes.begin(), plain_bytes.end());

    return true;
}

// ============ 会话管理 ============

// 获取与指定用户的会话状态
// 参数 peer_username: 对方用户名
// 返回值: 会话状态指针，不存在返回nullptr
SessionState* E2EEManager::getSession(const std::string& peer_username) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto it = sessions_.find(peer_username);
    if (it != sessions_.end()) {
        return &it->second;
    }
    return nullptr;
}

// 存储会话状态
// 参数 peer_username: 对方用户名
// 参数 session: 会话状态
void E2EEManager::storeSession(const std::string& peer_username, const SessionState& session) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    sessions_[peer_username] = session;
}

// 删除会话
// 参数 peer_username: 对方用户名
void E2EEManager::deleteSession(const std::string& peer_username) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    sessions_.erase(peer_username);
    Logger::info("[E2EE] 会话已删除: peer={}", peer_username);
}

// ============ 密钥分发协议 ============

// 生成密钥分发请求
// 参数 from_user: 请求方用户名
// 返回值: JSON 格式的密钥请求消息
std::string E2EEManager::createKeyRequest(const std::string& from_user) {
    nlohmann::json req{
        {"type", "e2ee_key_request"},
        {"from", from_user},
        {"timestamp", time(nullptr)}
    };
    return req.dump() + "\n";
}

// 生成密钥分发响应（包含己方公钥）
// 参数 device_id: 本设备ID
// 参数 kp: 本地密钥对（仅公钥部分会被发送）
// 返回值: JSON 格式的密钥响应消息
std::string E2EEManager::createKeyResponse(const std::string& device_id,
                                           const E2EEKeyPair& kp) {
    nlohmann::json resp{
        {"type", "e2ee_key_response"},
        {"device_id", device_id},
        {"key_data", kp.publicKeysToJson()},
        {"timestamp", time(nullptr)}
    };
    return resp.dump() + "\n";
}

// 处理收到的密钥分发响应并建立会话
// 解析对方公钥，执行 DH 密钥交换，存储会话状态
// 参数 from_user: 密钥来源用户
// 参数 json_response: 密钥响应 JSON（包含 key_data 字段）
// 返回值: 会话建立成功返回true
bool E2EEManager::handleKeyResponse(const std::string& from_user,
                                    const std::string& json_response) {
    auto j = nlohmann::json::parse(json_response);
    std::string key_data = j.value("key_data", "");
    if (key_data.empty()) {
        Logger::warn("[E2EE] 密钥响应缺少 key_data 字段");
        return false;
    }

    // 解析对方公钥
    E2EEKeyPair their_keys = E2EEKeyPair::publicKeysFromJson(key_data);

    // 获取本地密钥对（需要预先通过 generateIdentityKeys 生成）
    E2EEKeyPair our_keys;
    {
        std::lock_guard<std::mutex> lock(key_mutex_);
        // 使用当前用户作为 device_id
        if (key_pairs_.empty()) {
            Logger::error("[E2EE] 本地密钥对不存在，请先生成密钥");
            return false;
        }
        our_keys = key_pairs_.begin()->second;
    }

    // 执行 DH 密钥交换
    SessionState session;
    if (!performKeyExchange(our_keys, their_keys.identity_pub,
                            their_keys.signed_prekey_pub, session)) {
        return false;
    }

    // 存储会话
    storeSession(from_user, session);

    Logger::info("[E2EE] 会话已建立: peer={}", from_user);
    return true;
}

// ============ 内部方法 ============

// HKDF 密钥派生
// 使用 libsodium 的 crypto_kdf_derive_from_key 从输入密钥材料派生新密钥
// 参数 input_key: 输入密钥材料
// 参数 info: 上下文标签（用于域分离）
// 参数 output_key: 输出密钥（32 bytes）
void E2EEManager::hkdfDerive(const std::vector<uint8_t>& input_key,
                              const std::string& info,
                              std::vector<uint8_t>& output_key) {
    output_key.resize(AES256_GCM_KEY_BYTES);
    crypto_kdf_derive_from_key(
        output_key.data(), output_key.size(),
        1,                        // subkey_id
        info.c_str(),             // context
        input_key.data());
}

// 棘轮递进：从链密钥派生消息密钥并递进链密钥
// 使用 HMAC-SHA256 实现单向递进，保证前向安全性
// 参数 chain_key: 当前链密钥（输入且被更新为下一链密钥）
// 参数 message_key: 输出消息密钥（32 bytes，用于本次 AES 加密）
void E2EEManager::ratchetStep(std::vector<uint8_t>& chain_key,
                               std::vector<uint8_t>& message_key) {
    // message_key = HMAC-SHA256(chain_key, 0x01)
    // next_chain_key = HMAC-SHA256(chain_key, 0x02)
    const uint8_t MSG_KEY_CONSTANT = 0x01;
    const uint8_t CHAIN_KEY_CONSTANT = 0x02;

    message_key.resize(AES256_GCM_KEY_BYTES);
    std::vector<uint8_t> next_chain(AES256_GCM_KEY_BYTES);

    crypto_auth_hmacsha256_state state;

    // 计算消息密钥
    crypto_auth_hmacsha256_init(&state, chain_key.data(), chain_key.size());
    crypto_auth_hmacsha256_update(&state, &MSG_KEY_CONSTANT, 1);
    crypto_auth_hmacsha256_final(&state, message_key.data());

    // 计算下一链密钥
    crypto_auth_hmacsha256_init(&state, chain_key.data(), chain_key.size());
    crypto_auth_hmacsha256_update(&state, &CHAIN_KEY_CONSTANT, 1);
    crypto_auth_hmacsha256_final(&state, next_chain.data());

    chain_key = std::move(next_chain);
}

// ============ 密钥轮换 ============

// 检查密钥是否需要轮换（超过轮换周期）
// 参数 device_id: 设备ID
// 返回值: 需要轮换返回true
bool E2EEManager::needsKeyRotation(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(key_mutex_);
    auto it = key_pairs_.find(device_id);
    if (it == key_pairs_.end()) {
        // 没有密钥对，需要生成新的
        return true;
    }
    return it->second.ageSeconds() >= rotation_period_;
}

// 轮换指定设备的密钥对
// 生成新密钥对，更新内存和持久化存储
// 参数 device_id: 设备ID
// 参数 new_kp: 输出新生成的密钥对
// 返回值: 轮换成功返回true
bool E2EEManager::rotateKeyPair(const std::string& device_id, E2EEKeyPair& new_kp) {
    // 生成新密钥对
    new_kp = generateIdentityKeys(device_id);

    // 删除所有使用旧密钥建立的会话（需要重新进行密钥交换）
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sessions_.clear();
    }

    Logger::info("[E2EE] 密钥轮换完成: device={}, 新密钥已生成", device_id);
    return true;
}

// 设置密钥轮换周期（秒）
// 参数 seconds: 新的轮换周期
void E2EEManager::setRotationPeriod(int seconds) {
    if (seconds > 0) {
        rotation_period_ = seconds;
        Logger::info("[E2EE] 密钥轮换周期已更新: {} 秒", seconds);
    }
}

#endif // E2E_ENABLED
