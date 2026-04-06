#pragma once
// 三级缓存管理器：内存 LRU → Redis → MySQL
//
// 查询路径（短码 → 长 URL）：
//   1. 查 L1（内存 LRU）：命中 → 直接返回
//   2. 查 L2（Redis）：   命中 → 回填 L1，返回
//   3. 查 L3（MySQL）：   命中 → 回填 L1+L2，返回
//   4. 全部未命中 → 返回空字符串
//
// 写入路径（short code + long URL）：
//   写 L3（MySQL） → 写 L2（Redis） → 写 L1（内存）
//
// 删除路径：
//   删 L3 → 删 L2 → 删 L1

#include <string>
#include <vector>
#include <utility>
#include <memory>
#include "LRUCache.h"

class CacheManager {
public:
    // -------- 单例访问 --------
    static CacheManager& instance();

    // 初始化（需在 MySQLPool/RedisPool 初始化之后调用）
    void init();

    // 根据短码查询原始长 URL（返回空字符串表示不存在）
    std::string get(const std::string& code);

    // 保存短码与长 URL 的映射（写 MySQL → Redis → 内存）
    // 返回 false 表示 MySQL 写入失败
    bool put(const std::string& code, const std::string& longUrl);

    // 删除指定短码的所有缓存层数据
    bool remove(const std::string& code);

    // 从 MySQL 查询所有映射（用于前端展示，不走缓存）
    std::vector<std::pair<std::string, std::string>> listAll();

    // 分页查询（offset 从 0 开始，limit 为每页条数）
    std::vector<std::pair<std::string, std::string>> listPage(int offset, int limit);

    // 查询总条数
    long long countAll();

    // 根据长 URL 查找已有短码（用于去重，直接查 MySQL）
    std::string findByUrl(const std::string& normalizedUrl);

private:
    CacheManager()  = default;
    CacheManager(const CacheManager&)            = delete;
    CacheManager& operator=(const CacheManager&) = delete;

    // L1：内存 LRU 缓存（使用 unique_ptr，支持在 init() 中按配置重建）
    std::unique_ptr<LRUCache<std::string, std::string>> memCache_;

    // Redis TTL（秒），从配置读取
    int redisTTL_ = 3600;

    // ---- L2 Redis 操作 ----
    std::string redisGet(const std::string& code);
    void        redisSet(const std::string& code, const std::string& url, int ttl = -1);
    void        redisDel(const std::string& code);

    // ---- L3 MySQL 操作 ----
    std::string mysqlGet(const std::string& code);
    bool        mysqlInsert(const std::string& code, const std::string& longUrl);
    bool        mysqlDelete(const std::string& code);
};
