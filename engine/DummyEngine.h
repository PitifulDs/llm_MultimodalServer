#pragma once

#include "engine/LLMEngine.h"
#include <string>
#include <memory>

struct ServingContext;

// DummyEngine：用于验证 ServingContext 与 stream/non-stream 逻辑
class DummyEngine final : public LLMEngine
{
public:
    explicit DummyEngine(std::string text = "Hello") : text_(std::move(text))
    {
    }
    
    void Generate(ServingContext& ctx) override;
    void GenerateStream(ServingContext& ctx) override;

private:
    std::string text_;

};
