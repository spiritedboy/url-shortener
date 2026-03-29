#pragma once
// URL 短链转换器
// 算法：MurmurHash3 → Base62 编码 → 碰撞检测
//
// 确定性保证：
//   相同的（规范化后）长 URL 永远生成相同的短码
//   规范化：根据配置决定是否去掉 URL 的 ? 及后续内容
//
// 碰撞处理：
//   若生成的短码已映射到不同的 URL，则使用不同种子重新哈希（最多尝试 100 次）

#include <string>
#include <vector>
#include <utility>

class UrlShortener {
public:
    // -------- 单例访问 --------
    static UrlShortener& instance();

    // 初始化（读取配置）
    void init();

    // 将长 URL 转换为短码（确定性，相同输入返回相同短码）
    // 成功返回短码字符串，失败返回空字符串
    std::string shorten(const std::string& longUrl);

    // 根据短码查找对应的长 URL（返回空字符串表示不存在）
    std::string resolve(const std::string& code);

    // 删除短码映射
    bool remove(const std::string& code);

    // 列出所有映射对（code, originalUrl）
    std::vector<std::pair<std::string, std::string>> listAll();

    // 分页列出映射对（page 从 1 开始，pageSize 每页条数）
    // 同时返回总条数
    std::vector<std::pair<std::string, std::string>> listPage(
        int page, int pageSize, long long& total);

private:
    UrlShortener()  = default;
    UrlShortener(const UrlShortener&)            = delete;
    UrlShortener& operator=(const UrlShortener&) = delete;

    // 规范化 URL（根据配置决定是否忽略查询参数）
    std::string normalize(const std::string& url) const;

    // 生成候选短码（collision=0 表示首次，>0 表示碰撞重试）
    std::string generateCode(const std::string& normalizedUrl, int collision) const;

    int  codeLength_          = 6;
    bool ignoreQueryString_   = false;

    // 最大碰撞重试次数
    static const int MAX_COLLISION_RETRY = 100;
};
