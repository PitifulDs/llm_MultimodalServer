#pragma once
#include "serving/core/ServingContext.h"
#include "serving/core/ModelEngine.h"
#include "serving/service/EmbeddingsTypes.h"
#include "serving/service/RerankTypes.h"

#include <memory>
#include <mutex>
#include <string>

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
    bool RunEmbeddings(const EmbeddingsRequest &request,
                       EmbeddingsResponse &response,
                       std::string &error_message);
    bool RunRerank(const RerankRequest &request,
                   RerankResponse &response,
                   std::string &error_message);
    bool IsReady() const;

private:
    std::string model_path_;

    llama_model *model_ = nullptr;
    mutable std::mutex run_mu_;
    // llama_context *ctx_ = nullptr;
    // llama_sampler *sampler_ = nullptr;

    std::shared_ptr<ModelContext> EnsureContext(const std::shared_ptr<Session> &s);
    std::shared_ptr<ModelContext> CreateNewContext(bool embeddings = false,
                                                   int pooling_type = -1);
    bool EncodeTextEmbedding(const std::string &text,
                             std::vector<float> &embedding,
                             int &token_count,
                             std::string &error_message,
                             const std::shared_ptr<std::atomic<bool>> &cancelled = {},
                             std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
};
