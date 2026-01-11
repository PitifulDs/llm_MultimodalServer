#pragma once
#include <memory>

struct ServingContext;

class EngineExecutor
{
public:
    EngineExecutor() = default;
    ~EngineExecutor() = default;

    // 统一入口：一次请求执行（当前阶段=透明转发）
    void Execute(std::shared_ptr<ServingContext> ctx);
};
