#pragma once
// pthread 线程池
// - 固定数量的工作线程（由配置决定）
// - 任务队列：std::function<void()>
// - 线程同步：std::mutex + std::condition_variable
// - 优雅关闭：stop() 等待所有队列中任务完成后退出

#include <functional>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <pthread.h>

class ThreadPool {
public:
    explicit ThreadPool(int threadCount = 4);
    ~ThreadPool();

    // 提交一个任务到线程池队列
    // 任务类型：void()
    void submit(std::function<void()> task);

    // 等待所有任务执行完毕，然后关闭所有工作线程
    void stop();

    // 获取线程数量
    int threadCount() const { return static_cast<int>(threads_.size()); }

private:
    // 工作线程入口（pthread_create 要求 void* 返回）
    static void* workerEntry(void* arg);
    void workerLoop();

    std::vector<pthread_t>          threads_;    // 所有工作线程句柄
    std::queue<std::function<void()>> taskQueue_; // 待处理任务队列

    std::mutex              mutex_;
    std::condition_variable cond_;

    std::atomic<bool> running_{false};  // 线程池是否在运行
};
