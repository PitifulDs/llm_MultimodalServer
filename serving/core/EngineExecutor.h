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

#include "ThreadPool.h"

struct ServingContext;
class ModelEngine;

class EngineExecutor
{
public:
    EngineExecutor(ThreadPool &pool);
    ~EngineExecutor();

    // 异步：提交后立即返回（stream / non-stream 都走这条）
    bool Execute(std::shared_ptr<ServingContext> ctx);

    // 同步：用于 non-stream（内部 Execute + wait）
    void ExecuteAndWait(std::shared_ptr<ServingContext> ctx);

private:
    // ===== per-model queue =====
    struct ModelQueue
    {
        std::mutex mu;
        std::deque<std::function<void()>> tasks;
        bool running = false;
    };

    bool SubmitPerModel(const std::string &model, std::function<void()> task);
    void RunModelQueue(std::string model, std::shared_ptr<ModelQueue> mq);

private:
    std::shared_ptr<ModelEngine> GetOrCreateEngineLocked(const std::string &model);

    std::unordered_map<std::string, std::shared_ptr<ModelEngine>> engines_;

    ThreadPool& pool_;
    std::mutex map_mu_;
    std::unordered_map<std::string, std::shared_ptr<ModelQueue>> queues_;
};
