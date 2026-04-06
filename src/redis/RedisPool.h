#pragma once
// Redis 连接池
// - 使用 hiredis 库（hiredis/hiredis.h）
// - 连接池大小由配置文件指定
// - 使用互斥锁 + 条件变量保证线程安全
// - 提供 RAII Guard 机制自动归还连接

#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <ctime>
#include <hiredis/hiredis.h>

class RedisPool {
public:
    // -------- 单例访问 --------
    static RedisPool& instance();

    // 初始化连接池（读取 Config，创建所有连接）
    bool init();

    // 销毁所有连接
    void destroy();

    ~RedisPool();

    // -------- RAII 连接守卫 --------
    class Guard {
    public:
        explicit Guard(RedisPool& pool) : pool_(pool), ctx_(pool.acquire()) {}
        ~Guard() { pool_.release(ctx_); }

        redisContext* get()        const { return ctx_; }
        redisContext* operator->() const { return ctx_; }

        // 禁止拷贝
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        RedisPool&    pool_;
        redisContext* ctx_;
    };

private:
    RedisPool()  = default;
    RedisPool(const RedisPool&)            = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    // 借出一个连接（阻塞直到有可用连接）
    redisContext* acquire();

    // 归还一个连接
    void release(redisContext* ctx);

    // 创建单个 Redis 连接
    redisContext* createConnection();

    // 连接配置
    std::string host_;
    int         port_     = 6379;
    std::string password_;
    int         poolSize_ = 8;

    // 连接池
    std::vector<redisContext*>                   allCtxs_;    // 所有连接（用于析构销毁）
    std::queue<redisContext*>                    available_;  // 可用连接
    std::unordered_map<redisContext*, time_t>    lastUsed_;   // 连接最后归还时间
    std::mutex                                   mutex_;
    std::condition_variable                      cond_;

    static const int IDLE_TIMEOUT = 600; // 空闲超过此秒数才做 PING 检测
    bool initialized_ = false;
};
