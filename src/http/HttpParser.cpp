#include "HttpParser.h"
#include <algorithm>
#include <sstream>
#include <cctype>
#include <stdexcept>

// 去除首尾空白字符
std::string HttpParser::trim(const std::string& s) {
    const std::string ws = " \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

// URL 解码
std::string HttpParser::urlDecode(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            // 将两位十六进制转为对应字符
            int val = 0;
            for (int j = 1; j <= 2; ++j) {
                char c = s[i + j];
                val <<= 4;
                if (c >= '0' && c <= '9')      val |= (c - '0');
                else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
                else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
            }
            result += static_cast<char>(val);
            i += 2;
        } else if (s[i] == '+') {
            result += ' ';  // 表单编码中 + 代表空格
        } else {
            result += s[i];
        }
    }
    return result;
}

// 解析 HTTP 请求
bool HttpParser::parse(const std::string& buf, HttpRequest& req) {
    if (buf.empty()) return false;

    size_t pos = 0;

    // ---- 解析请求行 ----
    size_t lineEnd = buf.find("\r\n", pos);
    if (lineEnd == std::string::npos) return false;  // 请求行还未接收完

    std::string requestLine = buf.substr(pos, lineEnd - pos);
    pos = lineEnd + 2;

    // 拆分请求行的三个部分
    std::istringstream ss(requestLine);
    ss >> req.method >> req.path >> req.version;

    if (req.method.empty() || req.path.empty()) {
        throw std::runtime_error("非法 HTTP 请求行: " + requestLine);
    }

    // 统一请求方法为大写
    std::transform(req.method.begin(), req.method.end(), req.method.begin(), ::toupper);

    // 分离路径与查询字符串
    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        req.queryString = req.path.substr(qpos + 1);
        req.path        = req.path.substr(0, qpos);
    }

    // URL 解码路径
    req.path = urlDecode(req.path);

    // ---- 解析请求头 ----
    req.headers.clear();
    while (pos < buf.size()) {
        lineEnd = buf.find("\r\n", pos);
        if (lineEnd == std::string::npos) return false;  // 头部还未接收完

        if (lineEnd == pos) {
            // 空行：头部结束
            pos += 2;
            break;
        }

        std::string header = buf.substr(pos, lineEnd - pos);
        pos = lineEnd + 2;

        size_t colon = header.find(':');
        if (colon == std::string::npos) continue;  // 忽略格式不合法的头部

        std::string key   = trim(header.substr(0, colon));
        std::string value = trim(header.substr(colon + 1));

        // 头部键统一转为小写（方便后续大小写不敏感查找）
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        req.headers[key] = value;
    }

    // ---- 解析请求体 ----
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        size_t contentLength = 0;
        try {
            contentLength = std::stoul(it->second);
        } catch (...) {
            throw std::runtime_error("Content-Length 值非法: " + it->second);
        }

        if (buf.size() - pos < contentLength) {
            return false;  // 请求体还未接收完整
        }
        req.body = buf.substr(pos, contentLength);
    }

    req.complete = true;
    return true;
}
