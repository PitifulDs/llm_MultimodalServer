#pragma once
#include "serving/core/ServingContext.h"
#include "serving/core/ModelEngine.h"

#include <string>
#include <memory>

struct Session;      
struct ModelContext; 

struct llama_model;
// struct llama_context;
// struct llama_sampler;

class LlamaEngine final : public ModelEngine {
public:
    explicit LlamaEngine(const std::string& model_path);
    ~LlamaEngine() override;

    void Run(std::shared_ptr<ServingContext> ctx) override;

private:
    std::string model_path_;

    llama_model *model_ = nullptr;
    // llama_context *ctx_ = nullptr;
    // llama_sampler *sampler_ = nullptr;

    std::shared_ptr<ModelContext> EnsureContext(const std::shared_ptr<Session> &s);
    std::shared_ptr<ModelContext> CreateNewContext();
};