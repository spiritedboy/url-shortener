#pragma once
// Reactor 模式事件循环（epoll ET + EPOLLONESHOT 模式）
//
// 设计要点：
//   1. 主线程运行 epoll_wait，负责 accept 新连接和分发可读事件
//   2. 使用 ET（边沿触发）模式，必须循环读取直到 EAGAIN
//   3. 使用 EPOLLONESHOT 确保同一连接在同一时刻只由一个工作线程处理
//      → 工作线程处理完毕后，需调用 rearmConnection() 重新启用监听
//   4. 由 ThreadPool 提供工作线程执行业务逻辑
//   5. 事件循环本身运行在专属 pthread 中

#include <sys/epoll.h>
#include <unordered_map>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include "ThreadPool.h"

// 前向声明，避免循环依赖
class Connection;

class EventLoop {
public:
    // serverFd   : 监听 socket 的文件描述符
    // pool       : 共享工作线程池
    // handler    : 处理连接的回调（在工作线程中调用）
    // isAdmin    : true=管理接口连接，false=重定向接口连接
    EventLoop(int serverFd, ThreadPool* pool,
              std::function<void(std::shared_ptr<Connection>)> handler,
              bool isAdmin);
    ~EventLoop();

    // 启动事件循环（阻塞调用，直到 stop() 被调用）
    void run();

    // 停止事件循环
    void stop();

    // 重新注册连接的 EPOLLIN 事件（连接处理完毕且保持连接时调用）
    // 线程安全：可从工作线程调用
    void rearmConnection(int fd);

    // 关闭并移除连接（从 epoll 和 connections_ 中删除，关闭 fd）
    // 线程安全：可从工作线程调用
    void closeConnection(int fd);

private:
    // 单次 epoll_wait 处理的最大事件数
    static const int MAX_EVENTS = 1024;

    int            epollFd_;   // epoll 实例描述符
    int            serverFd_;  // 监听 socket
    std::atomic<bool> running_{false};

    ThreadPool* threadPool_;
    std::function<void(std::shared_ptr<Connection>)> handler_;
    bool           isAdmin_;   // 标记此 EventLoop 是否属于管理接口

    // fd → Connection 的映射（主线程写，工作线程读，需加锁）
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::mutex connMutex_;

    // 将 socket 设置为非阻塞模式
    static bool setNonBlocking(int fd);

    // 在 ET+ONESHOT 模式下接受所有等待的新连接（ET 要求循环 accept 直到 EAGAIN）
    void acceptAll();

    // 将 fd 加入 epoll 监听（ET + ONESHOT）
    void addFd(int fd, uint32_t events);
};
