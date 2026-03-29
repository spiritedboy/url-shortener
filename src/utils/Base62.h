#pragma once
// Base62 编码/解码工具
// 字符集：0-9 A-Z a-z（62 个字符）
// 用于将整数哈希值转换为短码字符串

#include <string>
#include <cstdint>
#include <stdexcept>

namespace Base62 {

// Base62 字符表：数字 + 大写字母 + 小写字母
static const char CHARS[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static const int BASE = 62;

// 将 64 位无符号整数编码为 Base62 字符串
// value  : 待编码的整数
// length : 目标字符串长度（不足时左填充 '0'，超出时截断高位）
inline std::string encode(uint64_t value, int length = 6) {
    std::string result(length, '0');
    for (int i = length - 1; i >= 0; --i) {
        result[i] = CHARS[value % BASE];
        value /= BASE;
    }
    return result;
}

// 将 Base62 字符串解码为 64 位无符号整数
// 若包含非法字符则抛出 std::invalid_argument
inline uint64_t decode(const std::string& s) {
    uint64_t result = 0;
    for (char c : s) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 36;

        if (digit < 0) {
            throw std::invalid_argument(std::string("非法 Base62 字符: ") + c);
        }
        result = result * BASE + static_cast<uint64_t>(digit);
    }
    return result;
}

// 判断字符串是否为合法的 Base62 编码
inline bool isValid(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            return false;
        }
    }
    return true;
}

} // namespace Base62
