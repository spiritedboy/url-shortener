#include "Connection.h"
#include "EventLoop.h"
#include "logger/Logger.h"
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sched.h>

Connection::Connection(int fd, EventLoop* loop, bool isAdmin)
    : fd_(fd), loop_(loop), isAdmin_(isAdmin) {
}

Connection::~Connection() {
    // fd 由 EventLoop::closeConnection 负责关闭，此处不重复关闭
}

// 从 socket 循环读取数据直到 EAGAIN（ET 模式必须读完所有数据）
bool Connection::readAll() {
    char buf[READ_CHUNK];

    while (true) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));

        if (n > 0) {
            // 追加到读缓冲区
            readBuf_.append(buf, static_cast<size_t>(n));
            // 防止恶意客户端发送超大请求导致内存耗尽（限制 8MB）
            if (readBuf_.size() > 8 * 1024 * 1024) {
                LOG_WARN("读缓冲区超过 8MB 上限，断开连接 fd=" + std::to_string(fd_));
                return false;
            }
        } else if (n == 0) {
            // 对端关闭连接（EOF）
            LOG_DEBUG("对端关闭连接 fd=" + std::to_string(fd_));
            return false;
        } else {
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 数据已读完，等待下次事件
                break;
            }
            if (errno == EINTR) {
                // 被信号中断，重试
                continue;
            }
            // 其他错误（连接重置等）
            LOG_DEBUG("read 错误 fd=" + std::to_string(fd_) +
                      " errno=" + strerror(errno));
            return false;
        }
    }

    return true;
}

// 发送写缓冲区的全部数据
bool Connection::sendAll() {
    int retryCount = 0;
    while (!writeBuf_.empty()) {
        ssize_t n = ::write(fd_, writeBuf_.data(), writeBuf_.size());

        if (n > 0) {
            // 移除已发送的部分
            writeBuf_.erase(0, static_cast<size_t>(n));
            retryCount = 0;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区已满，短暂让出 CPU 后重试
                if (++retryCount > 1000) {
                    LOG_WARN("write EAGAIN 重试次数过多 fd=" + std::to_string(fd_));
                    return false;
                }
                sched_yield();
                continue;
            }
            if (errno == EINTR) {
                continue;   // 被中断，重试
            }
            // 真正的发送错误
            LOG_DEBUG("write 错误 fd=" + std::to_string(fd_) +
                      " errno=" + strerror(errno));
            return false;
        }
    }

    return true;  // 全部发送完毕
}

// 设置要发送的响应（拷贝）
void Connection::setResponse(const std::string& resp) {
    writeBuf_ = resp;
}

// 设置要发送的响应（移动，避免拷贝）
void Connection::setResponse(std::string&& resp) {
    writeBuf_ = std::move(resp);
}
