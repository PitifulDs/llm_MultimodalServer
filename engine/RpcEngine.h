#pragma once
#include <memory>
#include <string>

#include "serving/core/ModelEngine.h"
#include "serving/core/ServingContext.h"

class StackFlowsClient;

/**
 * @brief RpcEngine
 *
 * 负责：
 * 1. 将 ServingContext 转换为 StackFlows RPC
 * 2. 处理 stream / non-stream
 * 3. 将结果通过 ctx->on_chunk / ctx->final_text 返回
 *
 * 不涉及 HTTP / SSE / JSON 输出
 */
class RpcEngine : public ModelEngine {
public:
    explicit RpcEngine(StackFlowsClient* client);
    void Run(std::shared_ptr<ServingContext> ctx) override;

private:
    StackFlowsClient *sf_client_; // 不持有
};
