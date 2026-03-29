#include "Logger.h"
#include <ctime>
#include <cstring>
#include <sstream>
#include <iostream>

// 获取当前时间字符串，格式: "2026-03-23 12:00:00"
std::string Logger::currentTimeStr() {
    time_t now = time(nullptr);
    struct tm tmBuf;
    localtime_r(&now, &tmBuf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return std::string(buf);
}

// 日志级别对应的字符串标签
const char* Logger::levelStr(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
    }
    return "UNKNW";
}

// 初始化日志模块，启动后台写线程
bool Logger::init(const std::string& filePath, LogLevel level) {
    filePath_ = filePath;
    minLevel_ = level;

    // 以追加模式打开日志文件
    ofs_.open(filePath, std::ios::app);
    if (!ofs_.is_open()) {
        // 无法打开文件时退化为标准错误输出警告（仅此处例外）
        std::cerr << "[Logger] 无法打开日志文件: " << filePath << std::endl;
        return false;
    }

    running_ = true;

    // 创建后台日志写入线程
    if (pthread_create(&thread_, nullptr, writeThread, this) != 0) {
        running_ = false;
        return false;
    }

    return true;
}

// 写入一条日志（线程安全，非阻塞地推入队列）
void Logger::log(LogLevel level, const std::string& msg) {
    // 过滤低于最小级别的日志
    if (static_cast<int>(level) < static_cast<int>(minLevel_)) {
        return;
    }

    // 拼装日志行：[时间] [级别] 消息
    std::string line = "[" + currentTimeStr() + "] [" + levelStr(level) + "] " + msg + "\n";

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push(std::move(line));
    }
    // 通知后台线程有新日志可写
    queueCond_.notify_one();
}

// 后台线程静态入口
void* Logger::writeThread(void* arg) {
    static_cast<Logger*>(arg)->writeLoop();
    return nullptr;
}

// 后台线程写循环
void Logger::writeLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(queueMutex_);
        // 等待新日志或停止信号
        queueCond_.wait(lock, [this] {
            return !queue_.empty() || !running_;
        });

        // 取出全部待写日志（减少锁持有时间）
        std::queue<std::string> localQueue;
        localQueue.swap(queue_);
        lock.unlock();

        // 批量写入文件
        while (!localQueue.empty()) {
            ofs_ << localQueue.front();
            localQueue.pop();
        }
        ofs_.flush();

        // 若已停止且队列已空，退出循环
        if (!running_) {
            std::lock_guard<std::mutex> guard(queueMutex_);
            if (queue_.empty()) break;
        }
    }
}

// 停止日志模块（刷写剩余日志后关闭）
void Logger::stop() {
    if (!running_) return;
    running_ = false;
    queueCond_.notify_all();
    pthread_join(thread_, nullptr);
    if (ofs_.is_open()) ofs_.close();
}

Logger::~Logger() {
    stop();
}

// 返回全局单例
Logger& Logger::instance() {
    static Logger inst;
    return inst;
}
