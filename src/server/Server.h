#pragma once
// 服务器主控类
// - 创建并管理两个 EventLoop：Admin 接口和 Redirect 跳转
// - 共用一个 ThreadPool
// - 每个 EventLoop 在独立的 pthread 中运行

#include <memory>
#include <pthread.h>
#include "AdminServer.h"
#include "RedirectServer.h"

// 前向声明
class ThreadPool;
class EventLoop;

class Server {
public:
    Server();
    ~Server();

    // 启动服务器（非阻塞：两个 EventLoop 均在独立线程中运行）
    void start();

    // 停止服务器（线程安全）
    void stop();

private:
    // EventLoop 线程入口结构（传递参数给 pthread）
    struct LoopArg {
        EventLoop* loop;
    };
    static void* loopThreadEntry(void* arg);

    // 创建监听 socket
    static int createServerSocket(int port);

    std::unique_ptr<ThreadPool>     threadPool_;
    std::unique_ptr<EventLoop>      adminLoop_;
    std::unique_ptr<EventLoop>      redirectLoop_;

    AdminServer    adminServer_;
    RedirectServer redirectServer_;

    pthread_t  redirectThread_{};   // 运行 redirectLoop_ 的线程
    bool       redirectThreadCreated_ = false;
    pthread_t  adminThread_{};      // 运行 adminLoop_ 的线程
    bool       adminThreadCreated_ = false;
};
