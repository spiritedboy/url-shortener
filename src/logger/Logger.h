#pragma once
// 异步文件日志模块
// - 支持 DEBUG / INFO / WARN / ERROR 四种级别
// - 日志消息通过队列异步写入文件，不阻塞调用线程
// - 后台 pthread 负责将队列数据刷写到文件
// - 所有输出仅写入日志文件，不打印到控制台

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <atomic>
#include <pthread.h>

// 日志级别枚举
enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3
};

class Logger {
public:
    // -------- 单例访问 --------
    static Logger& instance();

    // 初始化日志（打开文件，启动后台写线程）
    // filePath : 日志文件路径
    // level    : 最低输出级别（低于此级别的日志被丢弃）
    bool init(const std::string& filePath, LogLevel level = LogLevel::INFO);

    // 写入一条日志（线程安全）
    void log(LogLevel level, const std::string& msg);

    // 便捷方法
    void debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }
    void info (const std::string& msg) { log(LogLevel::INFO,  msg); }
    void warn (const std::string& msg) { log(LogLevel::WARN,  msg); }
    void error(const std::string& msg) { log(LogLevel::ERROR, msg); }

    // 停止后台线程，刷写剩余日志（程序退出前调用）
    void stop();

    ~Logger();

private:
    Logger()  = default;
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    // 后台写线程入口函数（pthread 要求 void* 返回类型）
    static void* writeThread(void* arg);
    void writeLoop();

    // 根据级别返回字符串标签
    static const char* levelStr(LogLevel level);

    // 获取当前时间字符串（格式: 2026-03-23 12:00:00）
    static std::string currentTimeStr();

    std::string     filePath_;
    LogLevel        minLevel_ = LogLevel::INFO;
    std::ofstream   ofs_;

    std::queue<std::string> queue_;      // 待写入日志队列
    std::mutex              queueMutex_;
    std::condition_variable queueCond_;

    pthread_t           thread_{};
    std::atomic<bool>   running_{false};
};

// -------- 便捷宏 --------
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
#define LOG_INFO(msg)  Logger::instance().info(msg)
#define LOG_WARN(msg)  Logger::instance().warn(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
