#include "Config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

// 去除字符串首尾的空白字符（空格、制表符、回车、换行）
std::string Config::trim(const std::string& s) {
    const std::string ws = " \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

// 加载并解析 INI 格式配置文件
bool Config::load(const std::string& filePath) {
    std::ifstream ifs(filePath);
    if (!ifs.is_open()) {
        return false;
    }

    std::string currentSection;
    std::string line;

    while (std::getline(ifs, line)) {
        // 去掉行内注释（; 后面的内容）
        size_t commentPos = line.find(';');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        // 去掉 # 风格注释
        commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }

        line = trim(line);
        if (line.empty()) continue;

        // 解析 [section] 标签
        if (line.front() == '[' && line.back() == ']') {
            currentSection = trim(line.substr(1, line.size() - 2));
            continue;
        }

        // 解析 key = value 键值对
        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key   = trim(line.substr(0, eqPos));
        std::string value = trim(line.substr(eqPos + 1));
        if (key.empty()) continue;

        // 拼接完整键名："section.key"
        std::string fullKey = currentSection.empty() ? key : currentSection + "." + key;
        data_[fullKey] = value;
    }

    return true;
}

// 获取字符串配置值
std::string Config::get(const std::string& section, const std::string& key,
                        const std::string& defaultVal) const {
    std::string fullKey = section.empty() ? key : section + "." + key;
    auto it = data_.find(fullKey);
    if (it == data_.end()) return defaultVal;
    return it->second;
}

// 获取整数配置值
int Config::getInt(const std::string& section, const std::string& key, int defaultVal) const {
    std::string val = get(section, key);
    if (val.empty()) return defaultVal;
    try {
        return std::stoi(val);
    } catch (...) {
        return defaultVal;
    }
}

// 获取布尔配置值
bool Config::getBool(const std::string& section, const std::string& key, bool defaultVal) const {
    std::string val = get(section, key);
    if (val.empty()) return defaultVal;
    // 统一转为小写后比较
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return (val == "true" || val == "1" || val == "yes");
}

// 返回全局单例
Config& Config::instance() {
    static Config inst;
    return inst;
}
