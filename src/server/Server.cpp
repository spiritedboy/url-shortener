#include "Server.h"
#include "EventLoop.h"
#include "ThreadPool.h"
#include "logger/Logger.h"
#include "config/Config.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

Server::Server() {
    // 从配置读取参数
    int adminPort    = Config::instance().getInt("server", "admin_port",    8080);
    int redirectPort = Config::instance().getInt("server", "redirect_port", 8000);
    int workerCount  = Config::instance().getInt("server", "worker_threads", 4);

    LOG_INFO("服务器配置: Admin=" + std::to_string(adminPort) +
             " Redirect=" + std::to_string(redirectPort) +
             " 工作线程=" + std::to_string(workerCount));

    // 初始化各子模块
    adminServer_.init();

    // 创建共享线程池
    threadPool_ = std::unique_ptr<ThreadPool>(new ThreadPool(workerCount));

    // 创建监听 socket
    int adminFd    = createServerSocket(adminPort);
    int redirectFd = createServerSocket(redirectPort);

    // 创建两个 EventLoop
    adminLoop_    = std::unique_ptr<EventLoop>(new EventLoop(
        adminFd, threadPool_.get(), adminServer_.getHandler(), true));

    redirectLoop_ = std::unique_ptr<EventLoop>(new EventLoop(
        redirectFd, threadPool_.get(), redirectServer_.getHandler(), false));
}

Server::~Server() {
    stop();
}

// 创建 TCP 监听 socket
int Server::createServerSocket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() 失败: " + std::string(strerror(errno)));
    }

    // 允许端口复用（防止重启时 TIME_WAIT 导致绑定失败）
    int optVal = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optVal, sizeof(optVal));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optVal, sizeof(optVal));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error(
            "bind() 失败 port=" + std::to_string(port) + ": " + strerror(errno));
    }

    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        throw std::runtime_error(
            "listen() 失败 port=" + std::to_string(port) + ": " + strerror(errno));
    }

    LOG_INFO("监听端口: " + std::to_string(port) + " fd=" + std::to_string(fd));
    return fd;
}

// EventLoop 线程入口
void* Server::loopThreadEntry(void* arg) {
    LoopArg* la = static_cast<LoopArg*>(arg);
    la->loop->run();
    delete la;
    return nullptr;
}

// 启动服务器（非阻塞：两个 EventLoop 均在独立线程中运行）
void Server::start() {
    LOG_INFO("服务器启动中...");

    // 在独立线程中运行 Redirect EventLoop
    LoopArg* arg1 = new LoopArg{ redirectLoop_.get() };
    if (pthread_create(&redirectThread_, nullptr, loopThreadEntry, arg1) != 0) {
        delete arg1;
        throw std::runtime_error("无法创建 Redirect EventLoop 线程");
    }
    redirectThreadCreated_ = true;

    // 在独立线程中运行 Admin EventLoop
    LoopArg* arg2 = new LoopArg{ adminLoop_.get() };
    if (pthread_create(&adminThread_, nullptr, loopThreadEntry, arg2) != 0) {
        delete arg2;
        throw std::runtime_error("无法创建 Admin EventLoop 线程");
    }
    adminThreadCreated_ = true;

    LOG_INFO("服务器已启动（双端口运行中）");
}

// 停止服务器
void Server::stop() {
    LOG_INFO("服务器停止中...");

    if (adminLoop_)    adminLoop_->stop();
    if (redirectLoop_) redirectLoop_->stop();

    // 等待两个 EventLoop 线程退出
    if (adminThreadCreated_) {
        pthread_join(adminThread_, nullptr);
        adminThreadCreated_ = false;
    }
    if (redirectThreadCreated_) {
        pthread_join(redirectThread_, nullptr);
        redirectThreadCreated_ = false;
    }

    // 停止线程池
    if (threadPool_) threadPool_->stop();

    LOG_INFO("服务器已停止");
}
