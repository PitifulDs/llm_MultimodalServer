#pragma once
#include "serving/core/ServingContext.h"
#include "serving/core/ModelEngine.h"

#include <string>
#include <memory>

struct llama_model;
struct llama_context;
struct llama_sampler;

class LlamaEngine final : public ModelEngine {
public:
    explicit LlamaEngine(const std::string& model_path);
    ~LlamaEngine();

    void Run(std::shared_ptr<ServingContext> ctx) override;

private:
    std::string model_path_;

    llama_model *model_ = nullptr;
    llama_context *ctx_ = nullptr;
    llama_sampler *sampler_ = nullptr;
};