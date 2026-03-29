#pragma once
// 重定向服务器（Redirect Server）
// 监听重定向端口（默认 8000/80），仅处理 GET /{code} 请求
// 找到对应长 URL 则返回 302 跳转，否则返回 404

#include <memory>
#include <functional>

class Connection;

class RedirectServer {
public:
    // 返回连接处理函数（传给 EventLoop）
    std::function<void(std::shared_ptr<Connection>)> getHandler();

private:
    // 实际处理连接（在工作线程中调用）
    void handleConnection(std::shared_ptr<Connection> conn);
};
