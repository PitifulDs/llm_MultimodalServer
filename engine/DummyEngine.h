#pragma once

#include "engine/LLMEngine.h"
#include "serving/core/ModelEngine.h" 
#include "serving/core/ServingContext.h"
#include <string>
#include <memory>

struct ServingContext;

// DummyEngine：用于验证 ServingContext 与 stream/non-stream 逻辑
class DummyEngine final : public ModelEngine
{
public:
    explicit DummyEngine(std::string text): text_(std::move(text)) 
    {
    }

    void Run(std::shared_ptr<ServingContext> ctx) override;

private:
    std::string text_;

};
