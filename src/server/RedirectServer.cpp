#include "RedirectServer.h"
#include "Connection.h"
#include "EventLoop.h"
#include "logger/Logger.h"
#include "shortener/UrlShortener.h"
#include "http/HttpParser.h"
#include "http/HttpResponse.h"
#include "utils/Base62.h"

// 返回处理函数
std::function<void(std::shared_ptr<Connection>)> RedirectServer::getHandler() {
    return [this](std::shared_ptr<Connection> conn) {
        handleConnection(conn);
    };
}

// 处理重定向连接（在工作线程中调用）
void RedirectServer::handleConnection(std::shared_ptr<Connection> conn) {
    // 读取全部数据
    if (!conn->readAll()) {
        conn->loop()->closeConnection(conn->fd());
        return;
    }

    if (conn->readBuf().empty()) {
        conn->loop()->rearmConnection(conn->fd());
        return;
    }

    // 解析 HTTP 请求
    HttpRequest req;
    std::string resp;

    try {
        bool complete = HttpParser::parse(conn->readBuf(), req);
        if (!complete) {
            // 请求不完整，继续等待
            conn->loop()->rearmConnection(conn->fd());
            return;
        }
    } catch (const std::exception& e) {
        LOG_WARN(std::string("Redirect 端 HTTP 解析失败: ") + e.what());
        resp = HttpResponse::badRequest();
        conn->setResponse(std::move(resp));
        conn->sendAll();
        conn->loop()->closeConnection(conn->fd());
        return;
    }

    conn->clearReadBuf();

    LOG_DEBUG("Redirect 请求: " + req.method + " " + req.path);

    // 只处理 GET 方法
    if (req.method != "GET") {
        resp = HttpResponse::methodNotAllowed();
        conn->setResponse(std::move(resp));
        conn->sendAll();
        conn->loop()->closeConnection(conn->fd());
        return;
    }

    // 从路径中提取短码
    // 路径格式：/{code}（例如 /abc123）
    std::string path = req.path;

    // 移除开头的 /
    if (!path.empty() && path[0] == '/') {
        path = path.substr(1);
    }

    // 根路径 / 返回提示信息
    if (path.empty()) {
        resp = HttpResponse::ok(
            "{\"message\":\"短链跳转服务，请访问 /{code} 进行跳转\"}");
        conn->setResponse(std::move(resp));
        conn->sendAll();
        conn->loop()->closeConnection(conn->fd());
        return;
    }

    // 校验短码格式（必须是合法的 Base62 字符）
    if (!Base62::isValid(path)) {
        resp = HttpResponse::notFound("无效的短码格式");
        conn->setResponse(std::move(resp));
        conn->sendAll();
        conn->loop()->closeConnection(conn->fd());
        return;
    }

    // 通过三级缓存查找对应的长 URL
    std::string longUrl = UrlShortener::instance().resolve(path);

    if (longUrl.empty()) {
        // 短码不存在
        resp = HttpResponse::notFound("短码不存在或已过期: " + path);
        LOG_DEBUG("短码不存在: " + path);
    } else {
        // 返回 302 跳转
        resp = HttpResponse::redirect(longUrl);
        LOG_INFO("302 跳转: " + path + " → " + longUrl);
    }

    conn->setResponse(std::move(resp));
    conn->sendAll();
    conn->loop()->closeConnection(conn->fd());
}
