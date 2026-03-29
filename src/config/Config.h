#pragma once
// INI 格式配置文件解析器
// 支持 [section] / key = value / ; 注释（行内注释也支持）

#include <string>
#include <unordered_map>

class Config {
public:
    // -------- 单例访问 --------
    static Config& instance();

    // 加载配置文件（返回 false 表示文件打开失败）
    bool load(const std::string& filePath);

    // 获取字符串配置值（不存在时返回 defaultVal）
    std::string get(const std::string& section, const std::string& key,
                    const std::string& defaultVal = "") const;

    // 获取整数配置值
    int getInt(const std::string& section, const std::string& key, int defaultVal = 0) const;

    // 获取布尔配置值（"true"/"1"/"yes" → true）
    bool getBool(const std::string& section, const std::string& key, bool defaultVal = false) const;

private:
    Config()  = default;
    ~Config() = default;
    Config(const Config&)            = delete;
    Config& operator=(const Config&) = delete;

    // 存储格式："section.key" → "value"
    std::unordered_map<std::string, std::string> data_;

    // 去除字符串首尾空白字符
    static std::string trim(const std::string& s);
};
