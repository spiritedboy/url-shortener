#include "RedisPool.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include <cstring>
#include <cstdlib>

RedisPool& RedisPool::instance() {
    static RedisPool inst;
    return inst;
}

// 初始化 Redis 连接池
bool RedisPool::init() {
    if (initialized_) return true;

    // 从配置文件读取参数
    host_     = Config::instance().get("redis", "host", "127.0.0.1");
    port_     = Config::instance().getInt("redis", "port", 6379);
    password_ = Config::instance().get("redis", "password", "");
    poolSize_ = Config::instance().getInt("redis", "pool_size", 8);

    LOG_INFO("初始化 Redis 连接池，地址: " + host_ + ":" + std::to_string(port_) +
             " 连接数: " + std::to_string(poolSize_));

    // 创建所有连接
    for (int i = 0; i < poolSize_; ++i) {
        redisContext* ctx = createConnection();
        if (!ctx) {
            LOG_ERROR("Redis 连接池初始化失败，第 " + std::to_string(i + 1) + " 个连接创建失败");
            return false;
        }
        allCtxs_.push_back(ctx);
        available_.push(ctx);
    }

    initialized_ = true;
    LOG_INFO("Redis 连接池初始化成功，共 " + std::to_string(poolSize_) + " 个连接");
    return true;
}

// 创建单个 Redis 连接并可选认证
redisContext* RedisPool::createConnection() {
    // 设置连接超时：3 秒
    struct timeval timeout = { 3, 0 };
    redisContext* ctx = redisConnectWithTimeout(host_.c_str(), port_, timeout);

    if (!ctx) {
        LOG_ERROR("Redis 连接分配失败（内存不足）");
        return nullptr;
    }

    if (ctx->err) {
        LOG_ERROR(std::string("Redis 连接失败: ") + ctx->errstr);
        redisFree(ctx);
        return nullptr;
    }

    // 如果配置了密码，发送 AUTH 命令
    if (!password_.empty()) {
        redisReply* reply = static_cast<redisReply*>(
            redisCommand(ctx, "AUTH %s", password_.c_str()));
        if (!reply) {
            LOG_ERROR("Redis AUTH 命令失败（连接异常）");
            redisFree(ctx);
            return nullptr;
        }
        if (reply->type == REDIS_REPLY_ERROR) {
            LOG_ERROR(std::string("Redis 认证失败: ") + reply->str);
            freeReplyObject(reply);
            redisFree(ctx);
            return nullptr;
        }
        freeReplyObject(reply);
    }

    return ctx;
}

// 借出一个连接（阻塞等待）
// 每次借出前发送 PING 检测存活，失败则自动重建
redisContext* RedisPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return !available_.empty(); });

    redisContext* ctx = available_.front();
    available_.pop();
    lock.unlock();

    // 检测连接是否存活
    bool alive = false;
    redisReply* reply = static_cast<redisReply*>(redisCommand(ctx, "PING"));
    if (reply && reply->type == REDIS_REPLY_STATUS &&
        std::strcmp(reply->str, "PONG") == 0) {
        alive = true;
    }
    if (reply) freeReplyObject(reply);

    if (!alive) {
        LOG_WARN("Redis 连接已断开，正在重建");
        redisContext* oldCtx = ctx;
        redisFree(ctx);
        ctx = createConnection();
        if (!ctx) {
            LOG_ERROR("Redis 连接重建失败");
        }
        // 更新 allCtxs_ 中的旧指针
        std::lock_guard<std::mutex> lock2(mutex_);
        for (auto& c : allCtxs_) {
            if (c == oldCtx) { c = ctx; break; }
        }
    }

    return ctx;
}

// 归还一个连接
void RedisPool::release(redisContext* ctx) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        available_.push(ctx);
    }
    cond_.notify_one();
}

// 销毁所有连接
void RedisPool::destroy() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (redisContext* ctx : allCtxs_) {
        redisFree(ctx);
    }
    allCtxs_.clear();
    while (!available_.empty()) available_.pop();
    initialized_ = false;
}

RedisPool::~RedisPool() {
    destroy();
}
