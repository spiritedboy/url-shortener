#include "EventLoop.h"
#include "Connection.h"
#include "logger/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <memory>

EventLoop::EventLoop(int serverFd, ThreadPool* pool,
                     std::function<void(std::shared_ptr<Connection>)> handler,
                     bool isAdmin)
    : serverFd_(serverFd), threadPool_(pool),
      handler_(std::move(handler)), isAdmin_(isAdmin) {

    // 创建 epoll 实例
    epollFd_ = epoll_create1(0);
    if (epollFd_ < 0) {
        throw std::runtime_error(std::string("epoll_create1 失败: ") + strerror(errno));
    }

    // 将监听 socket 设置为非阻塞（ET 模式要求）
    setNonBlocking(serverFd_);

    // 将监听 socket 加入 epoll（使用 ET 模式，接受新连接不需要 ONESHOT）
    struct epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = serverFd_;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, serverFd_, &ev) < 0) {
        close(epollFd_);
        throw std::runtime_error(std::string("epoll_ctl(ADD serverFd) 失败: ") + strerror(errno));
    }

    LOG_INFO(std::string("EventLoop 创建完成，类型: ") + (isAdmin_ ? "Admin" : "Redirect"));
}

EventLoop::~EventLoop() {
    if (epollFd_ >= 0) {
        close(epollFd_);
        epollFd_ = -1;
    }
}

// 设置 fd 为非阻塞模式（O_NONBLOCK）
bool EventLoop::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        LOG_ERROR(std::string("fcntl(F_GETFL) 失败: ") + strerror(errno));
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR(std::string("fcntl(F_SETFL) 失败: ") + strerror(errno));
        return false;
    }
    return true;
}

// 将 fd 加入 epoll，使用 ET + ONESHOT 模式
bool EventLoop::addFd(int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events  = events | EPOLLET | EPOLLONESHOT;
    ev.data.fd = fd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR(std::string("epoll_ctl(ADD) 失败 fd=") + std::to_string(fd) +
                  " errno=" + strerror(errno));
        return false;
    }
    return true;
}

// 循环 accept 所有等待的连接（ET 模式要求：不能只 accept 一个）
void EventLoop::acceptAll() {
    while (true) {
        // 检查连接数上限
        {
            std::lock_guard<std::mutex> lock(connMutex_);
            if (static_cast<int>(connections_.size()) >= MAX_CONNECTIONS) {
                LOG_WARN("连接数已达上限 " + std::to_string(MAX_CONNECTIONS) + "，拒绝新连接");
                // 把accept出来的立即关闭
                struct sockaddr_in tmpAddr{};
                socklen_t tmpLen = sizeof(tmpAddr);
                int tmpFd = accept(serverFd_, reinterpret_cast<struct sockaddr*>(&tmpAddr), &tmpLen);
                if (tmpFd >= 0) close(tmpFd);
                break;
            }
        }

        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);

        int connFd = accept(serverFd_,
                            reinterpret_cast<struct sockaddr*>(&clientAddr),
                            &addrLen);
        if (connFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有更多等待连接，正常退出
                break;
            }
            LOG_ERROR(std::string("accept 失败: ") + strerror(errno));
            break;
        }

        // 设置新连接为非阻塞
        if (!setNonBlocking(connFd)) {
            close(connFd);
            continue;
        }

        // 创建 Connection 对象并加入映射
        auto conn = std::make_shared<Connection>(connFd, this, isAdmin_);

        {
            std::lock_guard<std::mutex> lock(connMutex_);
            connections_[connFd] = conn;
        }

        // 将新 fd 加入 epoll（ET + ONESHOT，监听可读事件）
        if (!addFd(connFd, EPOLLIN)) {
            std::lock_guard<std::mutex> lock(connMutex_);
            connections_.erase(connFd);
            close(connFd);
            continue;
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        LOG_DEBUG(std::string("新连接: fd=") + std::to_string(connFd) +
                  " IP=" + ipStr + ":" + std::to_string(ntohs(clientAddr.sin_port)));
    }
}

// 主事件循环（阻塞运行）
void EventLoop::run() {
    struct epoll_event events[MAX_EVENTS];

    LOG_INFO(std::string("EventLoop 开始运行，类型: ") + (isAdmin_ ? "Admin" : "Redirect"));

    while (running_) {
        int n = epoll_wait(epollFd_, events, MAX_EVENTS, 500);  // 500ms 超时，便于检查 running_

        if (n < 0) {
            if (errno == EINTR) continue;  // 被信号中断，继续等待
            LOG_ERROR(std::string("epoll_wait 失败: ") + strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == serverFd_) {
                // 有新连接请求
                acceptAll();
                continue;
            }

            // 获取对应的 Connection
            std::shared_ptr<Connection> conn;
            {
                std::lock_guard<std::mutex> lock(connMutex_);
                auto it = connections_.find(fd);
                if (it == connections_.end()) continue;  // 连接已被移除
                conn = it->second;
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                // 连接异常，关闭
                LOG_DEBUG("连接异常，关闭 fd=" + std::to_string(fd));
                closeConnection(fd);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                // 有数据可读，提交到工作线程池处理
                // 使用 shared_ptr 确保 Connection 在任务执行期间不被销毁
                auto handlerCopy = handler_;
                threadPool_->submit([conn, handlerCopy]() {
                    handlerCopy(conn);
                });
            }
        }
    }

    LOG_INFO(std::string("EventLoop 已停止，类型: ") + (isAdmin_ ? "Admin" : "Redirect"));
}

// 停止事件循环
void EventLoop::stop() {
    running_ = false;
}

// 重新注册 EPOLLIN 事件（EPOLLONESHOT 触发后需要手动重置）
void EventLoop::rearmConnection(int fd) {
    // 检查连接是否仍然存在
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        if (connections_.find(fd) == connections_.end()) return;
    }

    struct epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = fd;
    // epoll_ctl 是线程安全的，可从工作线程调用
    if (epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        LOG_WARN(std::string("epoll_ctl(MOD) 失败 fd=") + std::to_string(fd) +
                 " errno=" + strerror(errno));
        closeConnection(fd);
    }
}

// 关闭并移除连接
void EventLoop::closeConnection(int fd) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        conn = it->second;
        connections_.erase(it);
    }

    // 从 epoll 移除
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    // 关闭文件描述符
    close(fd);
    LOG_DEBUG("关闭连接: fd=" + std::to_string(fd));
}
