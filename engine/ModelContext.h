#pragma once
#include <memory>

// llama forward declarations（只在 engine 层）
struct llama_context;
struct llama_sampler;

// Session 级模型运行状态（KV Cache 所在）
// 引擎私有，不属于 serving/core
struct ModelContext
{
    llama_context *ctx = nullptr;
    llama_sampler *sampler = nullptr;

    // KV 当前位置（已写入的 token 数）
    int n_past = 0;
    
    // 是否已经完成首轮 prefill
    bool initialized = false;

    ModelContext() = default;
    ModelContext(const ModelContext &) = delete;
    ModelContext &operator=(const ModelContext &) = delete;

    ~ModelContext(); // 在 .cc 中 free llama_context / sampler
};
