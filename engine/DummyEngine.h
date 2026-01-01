#pragma once

#include "serving/core/ModelEngine.h"
#include "serving/core/ServingContext.h"
#include <memory>

struct ServingContext;

// DummyEngine：用于验证 ServingContext 与 stream/non-stream 逻辑
class DummyEngine : public ModelEngine
{
public:
    void run(std::shared_ptr<ServingContext> ctx) override;

    // void RunStream(const ServingContext &ctx,
    //                const std::function<void(const std::string &)> &on_delta,
    //                const std::function<void()> &on_done) override;
};
