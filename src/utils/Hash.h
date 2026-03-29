#pragma once
// MurmurHash3 哈希算法（32 位变体）
// 特性：确定性、高分布性、低碰撞率，适用于短码生成
// 参考: https://github.com/aappleby/smhasher

#include <cstdint>
#include <string>
#include <cstring>

namespace Hash {

// MurmurHash3 32位实现
// key  : 待哈希数据
// len  : 数据长度（字节）
// seed : 哈希种子（默认 0）
// 返回 32 位哈希值
inline uint32_t murmur3_32(const void* key, int len, uint32_t seed = 0) {
    const uint8_t* data   = static_cast<const uint8_t*>(key);
    const int      nblocks = len / 4;

    uint32_t h1 = seed;

    // 常量
    const uint32_t c1 = 0xcc9e2d51u;
    const uint32_t c2 = 0x1b873593u;

    // 处理 4 字节块
    const uint32_t* blocks = reinterpret_cast<const uint32_t*>(data + nblocks * 4);
    for (int i = -nblocks; i; ++i) {
        uint32_t k1;
        memcpy(&k1, blocks + i, sizeof(k1));  // 防止未对齐读取

        k1 *= c1;
        k1  = (k1 << 15) | (k1 >> 17);        // ROTL32(k1, 15)
        k1 *= c2;

        h1 ^= k1;
        h1  = (h1 << 13) | (h1 >> 19);       // ROTL32(h1, 13)
        h1  = h1 * 5 + 0xe6546b64u;
    }

    // 处理尾部字节（< 4 字节）
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= static_cast<uint32_t>(tail[2]) << 16; // fall through
        case 2: k1 ^= static_cast<uint32_t>(tail[1]) << 8;  // fall through
        case 1: k1 ^= static_cast<uint32_t>(tail[0]);
                k1 *= c1;
                k1  = (k1 << 15) | (k1 >> 17);
                k1 *= c2;
                h1 ^= k1;
        default: break;
    }

    // Finalization mix（雪崩效应）
    h1 ^= static_cast<uint32_t>(len);
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6bu;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35u;
    h1 ^= h1 >> 16;

    return h1;
}

// 对字符串进行哈希（带可选种子）
inline uint32_t hashString(const std::string& s, uint32_t seed = 0) {
    return murmur3_32(s.data(), static_cast<int>(s.size()), seed);
}

} // namespace Hash
