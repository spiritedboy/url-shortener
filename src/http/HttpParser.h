#pragma once
// HTTP/1.x 请求解析器
// 支持：请求行、头部字段、请求体（Content-Length）
// 注意：不支持 chunked 传输编码（简化实现）
// 线程安全：无状态静态方法，可并发调用

#include <string>
#include <unordered_map>
#include <stdexcept>

// 解析后的 HTTP 请求结构
struct HttpRequest {
    std::string method;       // HTTP 方法：GET / POST / DELETE / OPTIONS
    std::string path;         // 请求路径（不含查询字符串）
    std::string queryString;  // 查询字符串（? 后面的内容）
    std::string version;      // HTTP 版本："HTTP/1.0" 或 "HTTP/1.1"

    // 所有请求头（键已统一转为小写，方便查找）
    std::unordered_map<std::string, std::string> headers;

    std::string body;     // 请求体
    bool complete = false; // true 表示请求已完整接收
};

class HttpParser {
public:
    // 尝试从缓冲区解析完整 HTTP 请求
    // 返回 true  → req 已填充完整请求
    // 返回 false → 数据不完整，需要继续接收
    // 抛出 std::runtime_error → 请求格式非法
    static bool parse(const std::string& buf, HttpRequest& req);

    // URL 解码（%XX 格式 → 字符）
    static std::string urlDecode(const std::string& s);

private:
    // 去除字符串首尾空白
    static std::string trim(const std::string& s);
};
