#include "ThreadPool.h"
#include "logger/Logger.h"
#include <stdexcept>

// 构造函数：创建指定数量的工作线程
ThreadPool::ThreadPool(int threadCount) {
    if (threadCount <= 0) threadCount = 1;

    running_ = true;

    for (int i = 0; i < threadCount; ++i) {
        pthread_t t;
        // 使用 pthread 创建工作线程（不使用 std::thread）
        if (pthread_create(&t, nullptr, workerEntry, this) != 0) {
            // 已创建的线程需要先停止
            running_ = false;
            cond_.notify_all();
            for (pthread_t& existing : threads_) {
                pthread_join(existing, nullptr);
            }
            threads_.clear();
            throw std::runtime_error("线程池：pthread_create 失败");
        }
        threads_.push_back(t);
    }

    LOG_INFO("线程池启动，工作线程数: " + std::to_string(threadCount));
}

// 析构函数：确保所有线程已停止
ThreadPool::~ThreadPool() {
    stop();
}

// 提交任务到线程池
void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;  // 线程池已关闭，拒绝新任务
        taskQueue_.push(std::move(task));
    }
    // 唤醒一个等待的工作线程
    cond_.notify_one();
}

// 停止线程池（等待所有当前任务完成）
void ThreadPool::stop() {
    if (!running_) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    // 唤醒所有等待中的工作线程，让它们检查 running_ 并退出
    cond_.notify_all();

    // 等待所有线程结束
    for (pthread_t& t : threads_) {
        pthread_join(t, nullptr);
    }
    threads_.clear();

    LOG_INFO("线程池已停止");
}

// pthread 静态入口（转发到成员函数）
void* ThreadPool::workerEntry(void* arg) {
    static_cast<ThreadPool*>(arg)->workerLoop();
    return nullptr;
}

// 工作线程主循环
void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 等待任务或停止信号
            cond_.wait(lock, [this] {
                return !taskQueue_.empty() || !running_;
            });

            // 线程池关闭且队列已空，退出循环
            if (!running_ && taskQueue_.empty()) {
                break;
            }

            // 取出一个任务
            if (!taskQueue_.empty()) {
                task = std::move(taskQueue_.front());
                taskQueue_.pop();
            }
        }

        // 在锁外执行任务，避免持锁时间过长
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                LOG_ERROR(std::string("工作线程任务执行异常: ") + e.what());
            } catch (...) {
                LOG_ERROR("工作线程任务执行未知异常");
            }
        }
    }
}
