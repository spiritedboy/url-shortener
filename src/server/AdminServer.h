#pragma once
// 管理接口服务器（Admin Server）
// 监听管理端口（默认 8080），提供：
//   GET  /                  → 返回前端 HTML 页面
//   GET  /api/links         → 获取所有短链映射列表（JSON）
//   POST /api/links         → 创建新短链（JSON body: {"url":"..."}）
//   DELETE /api/links/{code}→ 删除指定短链
//   GET  /api/config        → 返回前端所需配置（redirect_base_url 等）
//   OPTIONS /               → CORS 预检响应

#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <utility>

class Connection;

class AdminServer {
public:
    // 初始化（读取 frontend_dir 配置）
    void init();

    // 返回一个可传给 EventLoop 的连接处理函数
    std::function<void(std::shared_ptr<Connection>)> getHandler();

private:
    // 实际处理连接（在工作线程中调用）
    void handleConnection(std::shared_ptr<Connection> conn);

    // ---- 路由处理函数 ----
    std::string serveIndex();                          // GET /
    std::string handleListLinks(const std::string& queryString); // GET /api/links
    std::string handleAddLink(const std::string& body);// POST /api/links
    std::string handleDeleteLink(const std::string& code); // DELETE /api/links/{code}
    std::string handleGetConfig();                     // GET /api/config

    // ---- JSON 工具 ----
    // 从 JSON 字符串中提取指定 key 的 string 值
    static std::string extractJsonString(const std::string& json, const std::string& key);

    // 将映射对列表转为 JSON 数组字符串
    static std::string linksToJson(const std::vector<std::pair<std::string,std::string>>& links);

    std::string frontendDir_;    // 前端静态文件目录
};
