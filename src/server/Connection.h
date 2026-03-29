#pragma once
// 连接状态机
// 持有单个客户端连接的 fd、读写缓冲区、归属的 EventLoop 引用
//
// 生命周期：
//   EventLoop::acceptAll() 创建 → 工作线程处理请求 → 工作线程关闭/重置

#include <string>
#include <atomic>

// 前向声明
class EventLoop;

class Connection {
public:
    Connection(int fd, EventLoop* loop, bool isAdmin);
    ~Connection();

    // 从 socket 循环读取数据到 readBuf_（ET 模式：读至 EAGAIN 或连接关闭）
    // 返回 false 表示对端关闭连接（read 返回 0）或发生不可恢复错误
    bool readAll();

    // 尝试将 writeBuf_ 的数据全部发送出去（循环写至 EAGAIN 或全部发送完毕）
    // 返回 false 表示发生错误（不含 EAGAIN，那意味着需要继续发送）
    bool sendAll();

    // 设置要发送的响应内容
    void setResponse(const std::string& resp);
    void setResponse(std::string&& resp);

    // 获取读缓冲区（供 HTTP 解析器使用）
    const std::string& readBuf() const { return readBuf_; }

    // 清空读缓冲区（一次请求处理完毕后调用）
    void clearReadBuf() { readBuf_.clear(); }

    // 查询 writeBuf_ 是否还有未发送的数据
    bool hasPendingWrite() const { return !writeBuf_.empty(); }

    int        fd()      const { return fd_; }
    EventLoop* loop()    const { return loop_; }
    bool       isAdmin() const { return isAdmin_; }

private:
    int        fd_;
    EventLoop* loop_;
    bool       isAdmin_;

    std::string readBuf_;   // 接收缓冲区
    std::string writeBuf_;  // 待发送缓冲区

    // 单次读取块大小（4KB）
    static const size_t READ_CHUNK = 4096;
};
