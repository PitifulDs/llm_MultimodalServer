#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <atomic>

class ThreadPool
{
public:
    explicit ThreadPool(size_t n_threads);
    ~ThreadPool();

    void Submit(std::function<void()> fn);

private:
    void WorkerLoop();

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> q_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
};
