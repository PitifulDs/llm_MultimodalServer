#include "serving/core/ThreadPool.h"

ThreadPool::ThreadPool(size_t n_threads)
{
    workers_.reserve(n_threads);
    for (size_t i = 0; i < n_threads; ++i)
    {
        workers_.emplace_back([this]
                              { WorkerLoop(); });
    }
}

ThreadPool::~ThreadPool()
{
    stop_.store(true);
    cv_.notify_all();
    for (auto &t : workers_)
    {
        if (t.joinable())
            t.join();
    }
}

void ThreadPool::Submit(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.push(std::move(fn));
    }
    cv_.notify_one();
}

void ThreadPool::WorkerLoop()
{
    while (!stop_.load())
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&]{ return stop_.load() || !q_.empty(); });
            if (stop_.load() && q_.empty())
                return;
            task = std::move(q_.front());
            q_.pop();
        }
        task();
    }
}
