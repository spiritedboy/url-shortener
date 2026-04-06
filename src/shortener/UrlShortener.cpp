#include "UrlShortener.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "cache/CacheManager.h"
#include "utils/Hash.h"
#include "utils/Base62.h"
#include <sstream>

UrlShortener& UrlShortener::instance() {
    static UrlShortener inst;
    return inst;
}

// 从配置文件读取参数
void UrlShortener::init() {
    codeLength_        = Config::instance().getInt("shortener", "code_length", 6);
    ignoreQueryString_ = Config::instance().getBool("shortener", "ignore_query_string", false);

    LOG_INFO("UrlShortener 初始化：短码长度=" + std::to_string(codeLength_) +
             " 忽略查询参数=" + (ignoreQueryString_ ? "true" : "false"));
}

// 规范化 URL
// 根据配置决定是否去除 ? 及其后的查询参数部分
std::string UrlShortener::normalize(const std::string& url) const {
    if (!ignoreQueryString_) {
        return url;  // 保留完整 URL
    }
    // 找到 ? 的位置，截断
    size_t pos = url.find('?');
    if (pos == std::string::npos) return url;
    return url.substr(0, pos);
}

// 生成候选短码
// collision == 0：直接对规范化 URL 进行哈希
// collision  > 0：将碰撞计数附加到输入字符串后，改变种子重新哈希
std::string UrlShortener::generateCode(const std::string& normalizedUrl, int collision) const {
    // 将碰撞次数拼入输入，保证不同碰撞得到不同的哈希结果
    std::string input = normalizedUrl;
    if (collision > 0) {
        input += '\x00';  // 分隔符（防止 "abc" + "1" == "ab" + "c1"）
        input += std::to_string(collision);
    }

    // 使用 MurmurHash3 计算 32 位哈希值，种子为 0
    uint32_t h = Hash::hashString(input, 0);

    // 将哈希值扩展到 64 位（补充高位）以获得更多编码空间
    // 将 u32 的位反转拼到高 32 位
    uint64_t h64 = (static_cast<uint64_t>(h) << 32) | static_cast<uint64_t>(~h);

    return Base62::encode(h64, codeLength_);
}

// 将长 URL 转换为短码（公有接口）
std::string UrlShortener::shorten(const std::string& longUrl) {
    if (longUrl.empty()) {
        LOG_WARN("shorten 收到空 URL");
        return "";
    }

    if (longUrl.size() > 8192) {
        LOG_WARN("URL 超过 8192 字节上限，拒绝: 长度=" + std::to_string(longUrl.size()));
        return "";
    }

    // 对 URL 进行基本合法性检查（必须以 http:// 或 https:// 开头）
    if (longUrl.substr(0, 7) != "http://" && longUrl.substr(0, 8) != "https://") {
        LOG_WARN("URL 格式不合法（缺少协议头）: " + longUrl);
        return "";
    }

    std::string normed = normalize(longUrl);

    // 先在 MySQL 中查找是否已有该 URL 的短码（实现幂等）
    std::string existing = CacheManager::instance().findByUrl(normed);
    if (!existing.empty()) {
        LOG_DEBUG("URL 已存在短码: " + existing + " → " + normed);
        // 确保 L1/L2 中也有缓存
        CacheManager::instance().get(existing);  // 触发三级缓存预热
        return existing;
    }

    // 生成候选短码，处理碰撞
    for (int collision = 0; collision < MAX_COLLISION_RETRY; ++collision) {
        std::string code = generateCode(normed, collision);

        // 检查短码是否已被其他 URL 占用
        std::string existingUrl = CacheManager::instance().get(code);

        if (existingUrl.empty()) {
            // 短码未被占用，尝试写入
            if (CacheManager::instance().put(code, normed)) {
                LOG_INFO("新建短码: " + code + " → " + normed);
                return code;
            }
            // 写入失败（极少数情况，可能是并发写入同一 code）
            // 重新查找看是否同一 URL 写入了
            existingUrl = CacheManager::instance().get(code);
            if (existingUrl == normed) {
                return code;  // 并发写入了相同的映射，可以直接用
            }
            // 真的失败了，记录并继续
            LOG_WARN("短码写入失败: " + code);
            continue;
        }

        if (existingUrl == normed) {
            // 短码已存在且映射到相同 URL（幂等）
            LOG_DEBUG("短码已存在（相同 URL）: " + code);
            return code;
        }

        // 短码被不同 URL 占用 → 碰撞，尝试下一个
        LOG_DEBUG("短码碰撞（不同 URL），重试: collision=" + std::to_string(collision + 1));
    }

    LOG_ERROR("短码生成失败（超过最大碰撞重试次数）: " + longUrl);
    return "";
}

// 根据短码查找长 URL
std::string UrlShortener::resolve(const std::string& code) {
    if (code.empty() || !Base62::isValid(code)) {
        return "";
    }
    return CacheManager::instance().get(code);
}

// 删除短码映射
bool UrlShortener::remove(const std::string& code) {
    if (code.empty()) return false;
    LOG_INFO("删除短码: " + code);
    return CacheManager::instance().remove(code);
}

// 列出所有映射
std::vector<std::pair<std::string, std::string>> UrlShortener::listAll() {
    return CacheManager::instance().listAll();
}

// 分页列出映射（page 从 1 开始）
std::vector<std::pair<std::string, std::string>> UrlShortener::listPage(
        int page, int pageSize, long long& total) {
    if (page < 1) page = 1;
    if (pageSize < 1) pageSize = 20;
    if (pageSize > 200) pageSize = 200;
    total = CacheManager::instance().countAll();
    int offset = (page - 1) * pageSize;
    return CacheManager::instance().listPage(offset, pageSize);
}
