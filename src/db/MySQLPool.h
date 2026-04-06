#pragma once
// MySQL 连接池
// - 使用 libmysqlclient（mysql/mysql.h）
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
#include <mysql/mysql.h>

class MySQLPool {
public:
    // -------- 单例访问 --------
    static MySQLPool& instance();

    // 初始化连接池（读取 Config，创建所有连接，建库建表）
    bool init();

    // 销毁所有连接
    void destroy();

    ~MySQLPool();

    // -------- RAII 连接守卫 --------
    // 构造时借出连接，析构时归还
    class Guard {
    public:
        explicit Guard(MySQLPool& pool) : pool_(pool), conn_(pool.acquire()) {}
        ~Guard() { pool_.release(conn_); }

        MYSQL* get()        const { return conn_; }
        MYSQL* operator->() const { return conn_; }

        // 禁止拷贝
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        MySQLPool& pool_;
        MYSQL*     conn_;
    };

private:
    MySQLPool()  = default;
    MySQLPool(const MySQLPool&)            = delete;
    MySQLPool& operator=(const MySQLPool&) = delete;

    // 借出一个连接（阻塞直到有可用连接）
    MYSQL* acquire();

    // 归还一个连接
    void release(MYSQL* conn);

    // 创建并初始化单个 MySQL 连接
    MYSQL* createConnection();

    // 确保数据库和表结构存在
    bool ensureSchema(MYSQL* conn);

    // 连接配置
    std::string host_;
    int         port_     = 3306;
    std::string user_;
    std::string password_;
    std::string database_;
    int         poolSize_ = 8;

    // 连接池
    std::vector<MYSQL*>                      allConns_;   // 所有连接（用于析构时统一销毁）
    std::queue<MYSQL*>                       available_;  // 当前可用连接
    std::unordered_map<MYSQL*, time_t>       lastUsed_;   // 连接最后归还时间
    std::mutex                               mutex_;
    std::condition_variable                  cond_;

    static const int IDLE_TIMEOUT = 600; // 空闲超过此秒数才做 mysql_ping 检测
    bool initialized_ = false;
};
