#include "CacheManager.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "db/MySQLPool.h"
#include "redis/RedisPool.h"
#include <hiredis/hiredis.h>
#include <mysql/mysql.h>
#include <sstream>

CacheManager& CacheManager::instance() {
    static CacheManager inst;
    return inst;
}

void CacheManager::init() {
    // 读取内存缓存容量和 Redis TTL
    int memSize = Config::instance().getInt("memory_cache", "max_size", 10000);
    redisTTL_   = Config::instance().getInt("redis", "ttl", 3600);

    // 创建内存 LRU 缓存对象（由配置文件决定容量）
    memCache_.reset(new LRUCache<std::string, std::string>(static_cast<size_t>(memSize)));

    LOG_INFO("CacheManager 初始化完成，内存缓存容量: " + std::to_string(memSize) +
             " Redis TTL: " + std::to_string(redisTTL_) + "s");
}

// ============================================================
//  公共接口
// ============================================================

// 查询短码对应的长 URL（三级穿透查找）
std::string CacheManager::get(const std::string& code) {
    // L1：内存 LRU
    std::string url;
    if (memCache_ && memCache_->get(code, url)) {
        LOG_DEBUG("L1 命中: " + code);
        return url;
    }

    // L2：Redis
    url = redisGet(code);
    if (!url.empty()) {
        LOG_DEBUG("L2 命中: " + code);
        if (memCache_) memCache_->put(code, url);  // 回填 L1
        return url;
    }

    // L3：MySQL
    url = mysqlGet(code);
    if (!url.empty()) {
        LOG_DEBUG("L3 命中: " + code);
        redisSet(code, url);          // 回填 L2
        if (memCache_) memCache_->put(code, url);  // 回填 L1
        return url;
    }

    LOG_DEBUG("三级缓存未命中: " + code);
    return "";
}

// 写入短码与长 URL 映射到三级缓存
bool CacheManager::put(const std::string& code, const std::string& longUrl) {
    // 先写持久层 MySQL（若已存在相同 code，MySQL 唯一键冲突会导致失败）
    if (!mysqlInsert(code, longUrl)) {
        return false;
    }
    // 写 Redis
    redisSet(code, longUrl);
    // 写内存
    if (memCache_) memCache_->put(code, longUrl);
    return true;
}

// 删除短码从三级缓存
bool CacheManager::remove(const std::string& code) {
    bool ok = mysqlDelete(code);
    redisDel(code);
    if (memCache_) memCache_->remove(code);
    return ok;
}

// 列出 MySQL 中所有映射
std::vector<std::pair<std::string, std::string>> CacheManager::listAll() {
    std::vector<std::pair<std::string, std::string>> result;
    try {
        MySQLPool::Guard guard(MySQLPool::instance());
        MYSQL* conn = guard.get();

        if (mysql_query(conn, "SELECT `code`, `original_url` FROM `url_mappings` ORDER BY `created_at` DESC") != 0) {
            LOG_ERROR(std::string("MySQL 查询所有映射失败: ") + mysql_error(conn));
            return result;
        }

        MYSQL_RES* res = mysql_store_result(conn);
        if (!res) return result;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr) {
            if (row[0] && row[1]) {
                result.emplace_back(row[0], row[1]);
            }
        }
        mysql_free_result(res);

    } catch (const std::exception& e) {
        LOG_ERROR(std::string("listAll 异常: ") + e.what());
    }
    return result;
}

// 分页查询（offset 从 0 开始，limit 为每页条数）
std::vector<std::pair<std::string, std::string>> CacheManager::listPage(int offset, int limit) {
    std::vector<std::pair<std::string, std::string>> result;
    try {
        MySQLPool::Guard guard(MySQLPool::instance());
        MYSQL* conn = guard.get();

        std::string sql = "SELECT `code`, `original_url` FROM `url_mappings`"
                          " ORDER BY `created_at` DESC"
                          " LIMIT " + std::to_string(limit) +
                          " OFFSET " + std::to_string(offset);

        if (mysql_query(conn, sql.c_str()) != 0) {
            LOG_ERROR(std::string("MySQL 分页查询失败: ") + mysql_error(conn));
            return result;
        }

        MYSQL_RES* res = mysql_store_result(conn);
        if (!res) return result;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr) {
            if (row[0] && row[1]) {
                result.emplace_back(row[0], row[1]);
            }
        }
        mysql_free_result(res);

    } catch (const std::exception& e) {
        LOG_ERROR(std::string("listPage 异常: ") + e.what());
    }
    return result;
}

// 查询总条数
long long CacheManager::countAll() {
    try {
        MySQLPool::Guard guard(MySQLPool::instance());
        MYSQL* conn = guard.get();

        if (mysql_query(conn, "SELECT COUNT(*) FROM `url_mappings`") != 0) {
            LOG_ERROR(std::string("MySQL COUNT 查询失败: ") + mysql_error(conn));
            return 0;
        }

        MYSQL_RES* res = mysql_store_result(conn);
        if (!res) return 0;

        long long count = 0;
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) {
            count = std::stoll(row[0]);
        }
        mysql_free_result(res);
        return count;

    } catch (const std::exception& e) {
        LOG_ERROR(std::string("countAll 异常: ") + e.what());
        return 0;
    }
}

// 根据原始长 URL 查找已有短码（防重复）
std::string CacheManager::findByUrl(const std::string& normalizedUrl) {
    try {
        MySQLPool::Guard guard(MySQLPool::instance());
        MYSQL* conn = guard.get();

        // 使用 mysql_real_escape_string 防 SQL 注入
        std::string escaped(normalizedUrl.size() * 2 + 1, '\0');
        unsigned long len = mysql_real_escape_string(
            conn, &escaped[0], normalizedUrl.c_str(),
            static_cast<unsigned long>(normalizedUrl.size()));
        escaped.resize(len);

        std::string sql = "SELECT `code` FROM `url_mappings` WHERE `original_url` = '" + escaped + "' LIMIT 1";

        if (mysql_query(conn, sql.c_str()) != 0) {
            LOG_ERROR(std::string("MySQL 查询URL失败: ") + mysql_error(conn));
            return "";
        }

        MYSQL_RES* res = mysql_store_result(conn);
        if (!res) return "";

        std::string code;
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) {
            code = row[0];
        }
        mysql_free_result(res);
        return code;

    } catch (const std::exception& e) {
        LOG_ERROR(std::string("findByUrl 异常: ") + e.what());
        return "";
    }
}

// ============================================================
//  L2 Redis 操作
// ============================================================

std::string CacheManager::redisGet(const std::string& code) {
    try {
        RedisPool::Guard guard(RedisPool::instance());
        redisContext* ctx = guard.get();

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(ctx, "GET url:%s", code.c_str()));

        if (!reply) {
            LOG_WARN("Redis GET 无响应（连接可能断开）");
            return "";
        }

        std::string result;
        if (reply->type == REDIS_REPLY_STRING) {
            result = reply->str;
        }
        freeReplyObject(reply);
        return result;

    } catch (...) {
        LOG_WARN("Redis GET 操作失败: " + code);
        return "";
    }
}

void CacheManager::redisSet(const std::string& code, const std::string& url) {
    try {
        RedisPool::Guard guard(RedisPool::instance());
        redisContext* ctx = guard.get();

        redisReply* reply;
        if (redisTTL_ > 0) {
            // 带过期时间写入
            reply = static_cast<redisReply*>(
                redisCommand(ctx, "SETEX url:%s %d %s",
                             code.c_str(), redisTTL_, url.c_str()));
        } else {
            // 永不过期
            reply = static_cast<redisReply*>(
                redisCommand(ctx, "SET url:%s %s", code.c_str(), url.c_str()));
        }

        if (reply) freeReplyObject(reply);

    } catch (...) {
        LOG_WARN("Redis SET 操作失败: " + code);
    }
}

void CacheManager::redisDel(const std::string& code) {
    try {
        RedisPool::Guard guard(RedisPool::instance());
        redisContext* ctx = guard.get();

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(ctx, "DEL url:%s", code.c_str()));
        if (reply) freeReplyObject(reply);

    } catch (...) {
        LOG_WARN("Redis DEL 操作失败: " + code);
    }
}

// ============================================================
//  L3 MySQL 操作
// ============================================================

std::string CacheManager::mysqlGet(const std::string& code) {
    try {
        MySQLPool::Guard guard(MySQLPool::instance());
        MYSQL* conn = guard.get();

        // 转义短码（短码为 Base62，无特殊字符，此处为防御性处理）
        std::string escaped(code.size() * 2 + 1, '\0');
        unsigned long len = mysql_real_escape_string(
            conn, &escaped[0], code.c_str(),
            static_cast<unsigned long>(code.size()));
        escaped.resize(len);

        std::string sql = "SELECT `original_url` FROM `url_mappings` WHERE `code` = '" + escaped + "' LIMIT 1";

        if (mysql_query(conn, sql.c_str()) != 0) {
            LOG_ERROR(std::string("MySQL GET 查询失败: ") + mysql_error(conn));
            return "";
        }

        MYSQL_RES* res = mysql_store_result(conn);
        if (!res) return "";

        std::string url;
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) {
            url = row[0];
        }
        mysql_free_result(res);
        return url;

    } catch (...) {
        LOG_ERROR("MySQL GET 操作异常: " + code);
        return "";
    }
}

bool CacheManager::mysqlInsert(const std::string& code, const std::string& longUrl) {
    try {
        MySQLPool::Guard guard(MySQLPool::instance());
        MYSQL* conn = guard.get();

        // 转义两个参数（防止 SQL 注入）
        std::string escCode(code.size() * 2 + 1, '\0');
        unsigned long codeLen = mysql_real_escape_string(
            conn, &escCode[0], code.c_str(),
            static_cast<unsigned long>(code.size()));
        escCode.resize(codeLen);

        std::string escUrl(longUrl.size() * 2 + 1, '\0');
        unsigned long urlLen = mysql_real_escape_string(
            conn, &escUrl[0], longUrl.c_str(),
            static_cast<unsigned long>(longUrl.size()));
        escUrl.resize(urlLen);

        // INSERT IGNORE 遇到重复 code 时不报错（幂等）
        std::string sql = "INSERT IGNORE INTO `url_mappings` (`code`, `original_url`) VALUES ('"
                          + escCode + "', '" + escUrl + "')";

        if (mysql_query(conn, sql.c_str()) != 0) {
            LOG_ERROR(std::string("MySQL INSERT 失败: ") + mysql_error(conn));
            return false;
        }

        return true;

    } catch (...) {
        LOG_ERROR("MySQL INSERT 操作异常: " + code);
        return false;
    }
}

bool CacheManager::mysqlDelete(const std::string& code) {
    try {
        MySQLPool::Guard guard(MySQLPool::instance());
        MYSQL* conn = guard.get();

        std::string escaped(code.size() * 2 + 1, '\0');
        unsigned long len = mysql_real_escape_string(
            conn, &escaped[0], code.c_str(),
            static_cast<unsigned long>(code.size()));
        escaped.resize(len);

        std::string sql = "DELETE FROM `url_mappings` WHERE `code` = '" + escaped + "'";

        if (mysql_query(conn, sql.c_str()) != 0) {
            LOG_ERROR(std::string("MySQL DELETE 失败: ") + mysql_error(conn));
            return false;
        }

        return mysql_affected_rows(conn) > 0;

    } catch (...) {
        LOG_ERROR("MySQL DELETE 操作异常: " + code);
        return false;
    }
}
