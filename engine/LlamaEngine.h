#pragma once
#include "serving/core/ServingContext.h"
#include "engine/LLMEngine.h"
#include <string>

struct llama_model;
struct llama_context;
struct llama_sampler;

class LlamaEngine final : public LLMEngine {
public:
    explicit LlamaEngine(const std::string& model_path);
    ~LlamaEngine();

    void Generate(ServingContext& ctx) override;
    void GenerateStream(ServingContext& ctx) override;

private:
    std::string model_path_;

    llama_model *model_ = nullptr;
    llama_context *ctx_ = nullptr;
    llama_sampler *sampler_ = nullptr;
};