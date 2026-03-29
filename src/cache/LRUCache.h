#pragma once
// 内存 LRU 缓存（模板类，O(1) 读写/淘汰）
// 数据结构：std::list（双向链表）+ std::unordered_map（哈希表）
//
// - 链表头部存放最近访问的条目（MRU）
// - 链表尾部存放最久未访问的条目（LRU），淘汰时从尾部弹出
// - 哈希表存储 key → 链表迭代器，实现 O(1) 查找
// - 所有操作加互斥锁，线程安全

#include <list>
#include <unordered_map>
#include <mutex>
#include <utility>

template<typename K, typename V>
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {}

    // 查找键值
    // 成功返回 true 并将 value 设置为对应值，同时将其移至链表头部
    bool get(const K& key, V& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return false;

        // 将访问的节点移到链表头部（O(1) splice）
        list_.splice(list_.begin(), list_, it->second);
        value = it->second->second;
        return true;
    }

    // 插入或更新键值对
    // 若 key 已存在则更新值并移至头部；若容量已满则淘汰尾部最旧条目
    void put(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            // 更新已有条目，移至头部
            it->second->second = value;
            list_.splice(list_.begin(), list_, it->second);
            return;
        }

        // 容量已满，淘汰最久未访问的条目（链表尾部）
        if (list_.size() >= capacity_) {
            const K& lruKey = list_.back().first;
            map_.erase(lruKey);
            list_.pop_back();
        }

        // 插入新条目到链表头部
        list_.emplace_front(key, value);
        map_[key] = list_.begin();
    }

    // 删除指定键
    void remove(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return;
        list_.erase(it->second);
        map_.erase(it);
    }

    // 清空所有缓存条目
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.clear();
        map_.clear();
    }

    // 返回当前缓存条目数
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return list_.size();
    }

    // 返回缓存容量上限
    size_t capacity() const { return capacity_; }

private:
    using ListType = std::list<std::pair<K, V>>;
    using MapType  = std::unordered_map<K, typename ListType::iterator>;

    size_t   capacity_;
    ListType list_;          // 双向链表：头部 = 最近访问
    MapType  map_;           // 哈希表：快速定位链表节点
    mutable std::mutex mutex_;
};
