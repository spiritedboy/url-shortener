#include "MySQLPool.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include <sstream>
#include <stdexcept>
#include <cstring>

MySQLPool& MySQLPool::instance() {
    static MySQLPool inst;
    return inst;
}

// 初始化连接池
bool MySQLPool::init() {
    if (initialized_) return true;

    // 从配置文件读取连接参数
    host_     = Config::instance().get("mysql", "host", "127.0.0.1");
    port_     = Config::instance().getInt("mysql", "port", 3306);
    user_     = Config::instance().get("mysql", "user", "root");
    password_ = Config::instance().get("mysql", "password", "");
    database_ = Config::instance().get("mysql", "database", "url_shortener");
    poolSize_ = Config::instance().getInt("mysql", "pool_size", 8);

    LOG_INFO("初始化 MySQL 连接池，地址: " + host_ + ":" + std::to_string(port_) +
             " 数据库: " + database_ + " 连接数: " + std::to_string(poolSize_));

    // 创建所有连接
    for (int i = 0; i < poolSize_; ++i) {
        MYSQL* conn = createConnection();
        if (!conn) {
            LOG_ERROR("MySQL 连接池初始化失败，第 " + std::to_string(i + 1) + " 个连接创建失败");
            return false;
        }
        allConns_.push_back(conn);
        available_.push(conn);
        lastUsed_[conn] = time(nullptr);
    }

    // 使用第一个连接确保数据库和表存在
    if (!ensureSchema(allConns_[0])) {
        LOG_ERROR("MySQL 数据库模式初始化失败");
        return false;
    }

    initialized_ = true;
    LOG_INFO("MySQL 连接池初始化成功，共 " + std::to_string(poolSize_) + " 个连接");
    return true;
}

// 创建单个 MySQL 连接（先连不指定库，再建库选库）
MYSQL* MySQLPool::createConnection() {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        LOG_ERROR("mysql_init 失败");
        return nullptr;
    }

    // 设置连接超时（秒）
    unsigned int connectTimeout = 30;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    // 设置读写超时（秒）防止 mysql_query() 在远程服务器无响应时无限阻塞
    unsigned int rwTimeout = 30;
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT,  &rwTimeout);
    mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &rwTimeout);
    // 设置字符集（MySQL 8.0 已移除 my_bool，自动重连选项已弃用，跳过）
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    // 先连到服务器（不指定数据库），以便能执行 CREATE DATABASE
    if (!mysql_real_connect(conn, host_.c_str(), user_.c_str(),
                            password_.empty() ? nullptr : password_.c_str(),
                            nullptr, port_, nullptr, 0)) {
        LOG_ERROR(std::string("MySQL 连接失败: ") + mysql_error(conn));
        mysql_close(conn);
        return nullptr;
    }

    return conn;
}

// 确保数据库和表结构存在（幂等，可重复调用）
bool MySQLPool::ensureSchema(MYSQL* conn) {
    // 创建数据库（若不存在）
    std::string createDb = "CREATE DATABASE IF NOT EXISTS `" + database_ +
                           "` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci";
    if (mysql_query(conn, createDb.c_str()) != 0) {
        LOG_ERROR(std::string("创建数据库失败: ") + mysql_error(conn));
        return false;
    }

    // 切换到目标数据库
    if (mysql_select_db(conn, database_.c_str()) != 0) {
        LOG_ERROR(std::string("切换数据库失败: ") + mysql_error(conn));
        return false;
    }

    // 建表 SQL
    const char* createTable =
        "CREATE TABLE IF NOT EXISTS `url_mappings` ("
        "  `id`           BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "  `code`         VARCHAR(16)  NOT NULL COMMENT '短码',"
        "  `original_url` TEXT         NOT NULL COMMENT '原始长地址',"
        "  `created_at`   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE KEY `uk_code` (`code`),"
        "  INDEX `idx_created` (`created_at`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci"
        "  COMMENT='短链与长链映射表'";

    if (mysql_query(conn, createTable) != 0) {
        LOG_ERROR(std::string("创建表失败: ") + mysql_error(conn));
        return false;
    }

    // 保证所有连接都切换到目标数据库
    for (MYSQL* c : allConns_) {
        if (c != conn) {
            mysql_select_db(c, database_.c_str());
        }
    }

    LOG_INFO("MySQL 数据库模式初始化完成，数据库: " + database_);
    return true;
}

// 借出一个连接（若无可用连接则阻塞等待）
// 空闲超过 IDLE_TIMEOUT 才做 mysql_ping 检测，失效则自动重建
MYSQL* MySQLPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return !available_.empty(); });

    MYSQL* conn = available_.front();
    available_.pop();

    time_t lastTime = lastUsed_.count(conn) ? lastUsed_[conn] : 0;
    lock.unlock();

    // 只对空闲超过阈值的连接做健康检查
    if (time(nullptr) - lastTime > IDLE_TIMEOUT) {
        if (mysql_ping(conn) != 0) {
            LOG_WARN("连接已断开，正在重建: " + std::string(mysql_error(conn)));
            MYSQL* oldConn = conn;
            mysql_close(conn);
            conn = createConnection();
            if (conn) {
                mysql_select_db(conn, database_.c_str());
            } else {
                LOG_ERROR("MySQL 连接重建失败");
            }
            // 更新 allConns_ 和 lastUsed_ 中的旧指针
            std::lock_guard<std::mutex> lock2(mutex_);
            for (auto& c : allConns_) {
                if (c == oldConn) { c = conn; break; }
            }
            lastUsed_.erase(oldConn);
            if (conn) lastUsed_[conn] = time(nullptr);
        }
    }

    return conn;
}

// 归还一个连接到池中
void MySQLPool::release(MYSQL* conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastUsed_[conn] = time(nullptr);
        available_.push(conn);
    }
    cond_.notify_one();
}

// 销毁所有连接
void MySQLPool::destroy() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (MYSQL* conn : allConns_) {
        mysql_close(conn);
    }
    allConns_.clear();
    lastUsed_.clear();
    while (!available_.empty()) available_.pop();
    initialized_ = false;
}

MySQLPool::~MySQLPool() {
    destroy();
}
