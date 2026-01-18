#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <functional>

#include "serving/core/ServingContext.h"

// llama forward declarations（避免在头文件 include llama.h）
struct llama_context;
struct llama_sampler;

// ModelContext
// Session 级模型运行状态（KV Cache 所在）
struct ModelContext
{
    // Session 独享
    llama_context *ctx = nullptr;
    llama_sampler *sampler = nullptr;

    // 语义位：是否已完成首轮 prefill
    bool initialized = false;

    // 禁止 copy
    ModelContext() = default;
    ModelContext(const ModelContext &) = delete;
    ModelContext &operator=(const ModelContext &) = delete;

    ~ModelContext(); // 在 .cc 里 free llama_context / sampler
};


struct  Session
{
    using Clock = std::chrono::steady_clock;

    explicit Session(std::string session_id, std::string model_name)
        : session_id(std::move(session_id)), model(std::move(model_name))
    {
    }

    std::string session_id;
    std::string model;

    // runtime state
    std::shared_ptr<ModelContext> model_ctx;    // kv chae / llm ctx
    std::vector<Message> history;               // 多轮对话历史

    Clock::time_point created_at{Clock::now()};
    Clock::time_point last_active{Clock::now()};
    bool closed{false};

    std::deque<std::function<void()>> pending;
    bool running{false};

    // 如果希望“同一 session 同时只能跑一个请求”，就用这个锁在 Engine 入口处串行化
    mutable std::mutex mu;

    void touch(){
        last_active = Clock::now();
    }
};
