#include "AdminServer.h"
#include "Connection.h"
#include "EventLoop.h"
#include "logger/Logger.h"
#include "config/Config.h"
#include "shortener/UrlShortener.h"
#include "http/HttpParser.h"
#include "http/HttpResponse.h"
#include "utils/Base62.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <utility>

// 初始化：读取前端目录配置
void AdminServer::init() {
    frontendDir_ = Config::instance().get("server", "frontend_dir", "./frontend");
    LOG_INFO("AdminServer 初始化，前端目录: " + frontendDir_);
}

// 返回处理函数（lambda 捕获 this）
std::function<void(std::shared_ptr<Connection>)> AdminServer::getHandler() {
    return [this](std::shared_ptr<Connection> conn) {
        handleConnection(conn);
    };
}

// 处理一个 HTTP 连接（在工作线程中调用）
void AdminServer::handleConnection(std::shared_ptr<Connection> conn) {
    // 从 socket 读取所有数据（ET 模式，循环读到 EAGAIN）
    if (!conn->readAll()) {
        conn->loop()->closeConnection(conn->fd());
        return;
    }

    if (conn->readBuf().empty()) {
        // 没有数据，重新注册监听等待
        conn->loop()->rearmConnection(conn->fd());
        return;
    }

    // 解析 HTTP 请求
    HttpRequest req;
    std::string resp;

    try {
        bool complete = HttpParser::parse(conn->readBuf(), req);
        if (!complete) {
            // 请求不完整，等待更多数据
            conn->loop()->rearmConnection(conn->fd());
            return;
        }
    } catch (const std::exception& e) {
        LOG_WARN(std::string("HTTP 解析失败: ") + e.what());
        resp = HttpResponse::badRequest();
        conn->setResponse(std::move(resp));
        conn->sendAll();
        conn->loop()->closeConnection(conn->fd());
        return;
    }

    conn->clearReadBuf();

    LOG_DEBUG("Admin 请求: " + req.method + " " + req.path);

    // ---- CORS 预检 ----
    if (req.method == "OPTIONS") {
        conn->setResponse(HttpResponse::options());
        conn->sendAll();
        conn->loop()->closeConnection(conn->fd());
        return;
    }

    // ---- 路由分发 ----
    if (req.path == "/" && req.method == "GET") {
        resp = serveIndex();

    } else if (req.path == "/api/links" && req.method == "GET") {
        resp = handleListLinks(req.queryString);

    } else if (req.path == "/api/config" && req.method == "GET") {
        resp = handleGetConfig();

    } else if (req.path == "/api/links" && req.method == "POST") {
        resp = handleAddLink(req.body);

    } else if (req.path.size() > 11 &&
               req.path.substr(0, 11) == "/api/links/" &&
               req.method == "DELETE") {
        std::string code = req.path.substr(11);
        if (!Base62::isValid(code)) {
            resp = HttpResponse::badRequest("短码格式不合法");
        } else {
            resp = handleDeleteLink(code);
        }

    } else if (req.path == "/api/links" && req.method == "DELETE") {
        // 路径参数在 query string 中的 fallback（兼容某些前端）
        std::string code;
        // 尝试从查询字符串取 code 参数
        auto& qs = req.queryString;
        std::string prefix = "code=";
        size_t pos = qs.find(prefix);
        if (pos != std::string::npos) {
            code = qs.substr(pos + prefix.size());
        }
        resp = code.empty() ? HttpResponse::badRequest("缺少短码参数")
                            : handleDeleteLink(code);

    } else {
        resp = HttpResponse::notFound("接口不存在");
    }

    conn->setResponse(std::move(resp));
    conn->sendAll();
    conn->loop()->closeConnection(conn->fd());
}

// ---- GET /api/config：返回前端所需配置 ----
std::string AdminServer::handleGetConfig() {
    std::string baseUrl = Config::instance().get("server", "redirect_base_url", "");
    // 去掉末尾多余的斜杠
    while (!baseUrl.empty() && baseUrl.back() == '/') {
        baseUrl.pop_back();
    }
    // 对值进行基本 JSON 字符串转义
    std::string escaped;
    for (char c : baseUrl) {
        if (c == '"')  escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else escaped += c;
    }
    std::string json = "{\"success\":true,\"redirect_base_url\":\"" + escaped + "\"}";
    return HttpResponse::ok(json);
}

// ---- 返回前端 HTML 页面 ----
std::string AdminServer::serveIndex() {
    std::string path = frontendDir_ + "/index.html";
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        LOG_ERROR("无法打开前端文件: " + path);
        return HttpResponse::notFound("前端文件不存在: " + path);
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return HttpResponse::ok(oss.str(), "text/html");
}

// ---- GET /api/links：返回分页短链列表 ----
std::string AdminServer::handleListLinks(const std::string& queryString) {
    // 解析查询参数 page 和 page_size
    auto getParam = [&](const std::string& key, int defaultVal) -> int {
        std::string search = key + "=";
        size_t pos = queryString.find(search);
        if (pos == std::string::npos) return defaultVal;
        pos += search.size();
        size_t end = queryString.find('&', pos);
        std::string val = (end == std::string::npos)
                          ? queryString.substr(pos)
                          : queryString.substr(pos, end - pos);
        try { return std::stoi(val); } catch (...) { return defaultVal; }
    };

    int page     = getParam("page",      1);
    int pageSize = getParam("page_size", 20);

    long long total = 0;
    auto links = UrlShortener::instance().listPage(page, pageSize, total);

    // 返回分页信息 + 数据
    std::string json = "{\"success\":true,"
                       "\"total\":"     + std::to_string(total)    + ","
                       "\"page\":"      + std::to_string(page)     + ","
                       "\"page_size\":" + std::to_string(pageSize) + ","
                       "\"data\":"      + linksToJson(links)       + "}";
    return HttpResponse::ok(json);
}

// ---- POST /api/links：创建短链 ----
std::string AdminServer::handleAddLink(const std::string& body) {
    if (body.empty()) {
        return HttpResponse::badRequest("请求体不能为空");
    }

    // 从 JSON body 提取 "url" 字段
    std::string url = extractJsonString(body, "url");
    if (url.empty()) {
        return HttpResponse::badRequest("缺少 url 字段");
    }

    // 调用短链生成器
    std::string code = UrlShortener::instance().shorten(url);
    if (code.empty()) {
        return HttpResponse::serverError("短链生成失败，请检查 URL 格式");
    }

    // 构造管理端口的访问地址
    int redirectPort = Config::instance().getInt("server", "redirect_port", 8000);

    std::string shortUrl = "http://localhost:" + std::to_string(redirectPort) + "/" + code;

    std::string json = "{\"success\":true,\"data\":{\"code\":\""
                       + HttpResponse::jsonEscape(code) + "\",\"url\":\""
                       + HttpResponse::jsonEscape(url) + "\",\"short_url\":\""
                       + HttpResponse::jsonEscape(shortUrl) + "\"}}";

    LOG_INFO("创建短链: " + code + " → " + url);
    return HttpResponse::ok(json);
}

// ---- DELETE /api/links/{code}：删除短链 ----
std::string AdminServer::handleDeleteLink(const std::string& code) {
    if (code.empty()) {
        return HttpResponse::badRequest("短码不能为空");
    }

    bool ok = UrlShortener::instance().remove(code);
    if (!ok) {
        return HttpResponse::notFound("短码不存在: " + code);
    }

    LOG_INFO("删除短链: " + code);
    return HttpResponse::ok("{\"success\":true}");
}

// ---- 从 JSON 中提取字符串字段（简易解析，不依赖第三方库）----
std::string AdminServer::extractJsonString(const std::string& json, const std::string& key) {
    // 查找 "key":
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    pos += searchKey.size();

    // 跳过空白字符和冒号
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                  json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != ':') return "";
    ++pos;

    // 再次跳过空白
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                  json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return "";
    ++pos;  // 跳过开始引号

    // 读取字符串值，处理转义序列
    std::string value;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case '"':  value += '"';  break;
                case '\\': value += '\\'; break;
                case '/':  value += '/';  break;
                case 'n':  value += '\n'; break;
                case 'r':  value += '\r'; break;
                case 't':  value += '\t'; break;
                default:   value += json[pos]; break;
            }
        } else {
            value += json[pos];
        }
        ++pos;
    }
    return value;
}

// ---- 将映射列表序列化为 JSON 数组 ----
std::string AdminServer::linksToJson(const std::vector<std::pair<std::string,std::string>>& links) {
    std::string arr = "[";
    for (size_t i = 0; i < links.size(); ++i) {
        if (i > 0) arr += ",";
        arr += "{\"code\":\"" + HttpResponse::jsonEscape(links[i].first) + "\","
               "\"url\":\""   + HttpResponse::jsonEscape(links[i].second) + "\"}";
    }
    arr += "]";
    return arr;
}
