#pragma once

#include <functional>
#include <string>
#include "serving/core/ServingContext.h"
#include <memory>
struct ServingContext;

// 统一的模型执行抽象（与具体模型实现解耦）
class ModelEngine
{
public:
    virtual ~ModelEngine() = default;

    // 非流式：返回完整文本
    virtual void Run(std::shared_ptr<ServingContext> ctx) = 0;

    // // 流式：按 token 回调输出
    // virtual void RunStream(const ServingContext &ctx,
    //                        const std::function<void(const std::string &)> &on_delta,
    //                        const std::function<void()> &on_done) = 0;
};
