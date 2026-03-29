#pragma once
// HTTP 响应构建工具
// 提供常用状态码的静态构建方法
// 所有响应均包含 CORS 头，允许前端跨域调用

#include <string>

class HttpResponse {
public:
    // 200 OK
    static std::string ok(const std::string& body,
                          const std::string& contentType = "application/json") {
        return build(200, "OK", contentType, body);
    }

    // 302 临时重定向
    static std::string redirect(const std::string& location) {
        std::string resp  = "HTTP/1.1 302 Found\r\n";
        resp += "Location: " + location + "\r\n";
        resp += "Content-Length: 0\r\n";
        resp += "Connection: close\r\n";
        resp += "\r\n";
        return resp;
    }

    // 400 Bad Request
    static std::string badRequest(const std::string& msg = "请求格式错误") {
        return build(400, "Bad Request", "application/json",
                     "{\"success\":false,\"message\":\"" + jsonEscape(msg) + "\"}");
    }

    // 404 Not Found
    static std::string notFound(const std::string& msg = "资源不存在") {
        return build(404, "Not Found", "application/json",
                     "{\"success\":false,\"message\":\"" + jsonEscape(msg) + "\"}");
    }

    // 405 Method Not Allowed
    static std::string methodNotAllowed() {
        return build(405, "Method Not Allowed", "application/json",
                     "{\"success\":false,\"message\":\"不支持的请求方法\"}");
    }

    // 500 Internal Server Error
    static std::string serverError(const std::string& msg = "服务器内部错误") {
        return build(500, "Internal Server Error", "application/json",
                     "{\"success\":false,\"message\":\"" + jsonEscape(msg) + "\"}");
    }

    // OPTIONS 预检响应（CORS）
    static std::string options() {
        std::string resp = "HTTP/1.1 204 No Content\r\n";
        resp += "Access-Control-Allow-Origin: *\r\n";
        resp += "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
        resp += "Access-Control-Allow-Headers: Content-Type\r\n";
        resp += "Access-Control-Max-Age: 86400\r\n";
        resp += "Content-Length: 0\r\n";
        resp += "Connection: close\r\n";
        resp += "\r\n";
        return resp;
    }

    // JSON 特殊字符转义（防止注入到 JSON 字符串中）
    static std::string jsonEscape(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (unsigned char c : s) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n";  break;
                case '\r': result += "\\r";  break;
                case '\t': result += "\\t";  break;
                default:
                    if (c < 0x20) {
                        // 控制字符使用 \uXXXX 转义
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        result += buf;
                    } else {
                        result += static_cast<char>(c);
                    }
            }
        }
        return result;
    }

private:
    // 组装标准 HTTP/1.1 响应
    static std::string build(int status, const std::string& statusText,
                             const std::string& contentType,
                             const std::string& body) {
        std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n";
        resp += "Content-Type: " + contentType;
        if (contentType.find("text/") == 0) {
            resp += "; charset=utf-8";
        }
        resp += "\r\n";
        resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";

        // CORS 头（允许前端跨域调用）
        resp += "Access-Control-Allow-Origin: *\r\n";
        resp += "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
        resp += "Access-Control-Allow-Headers: Content-Type\r\n";

        resp += "Connection: close\r\n";
        resp += "\r\n";
        resp += body;
        return resp;
    }
};
