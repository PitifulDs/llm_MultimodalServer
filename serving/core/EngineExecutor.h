#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <atomic>
#include <thread>

struct ServingContext;

class EngineExecutor
{
public:
    EngineExecutor(size_t worker_threads = 4);
    ~EngineExecutor();

    // 异步：提交后立即返回（stream / non-stream 都走这条）
    bool Execute(std::shared_ptr<ServingContext> ctx);

    // 同步：用于 non-stream（内部 Execute + wait）
    void ExecuteAndWait(std::shared_ptr<ServingContext> ctx);

private:
    // ===== ThreadPool (minimal) =====
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
        std::deque<std::function<void()>> q_;
        std::vector<std::thread> workers_;
        std::atomic<bool> stop_{false};
    };

    // ===== per-model queue =====
    struct ModelQueue
    {
        std::mutex mu;
        std::deque<std::function<void()>> tasks;
        bool running = false;
    };

    bool SubmitPerModel(const std::string &model, std::function<void()> task);
    void RunModelQueue(const std::string model, std::shared_ptr<ModelQueue> mq);

private:
    ThreadPool pool_;

    std::mutex map_mu_;
    std::unordered_map<std::string, std::shared_ptr<ModelQueue>> queues_;
};
