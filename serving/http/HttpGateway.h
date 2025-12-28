#pragma once
#include <memory>
#include <string>
#include "protocol/Protocol.h"

// 前向声明
struct HttpRequest;
struct HttpResponse;
class StackFlowsClient;
// 新增
struct ServingContext;
class ModelEngine;

/**
 * @brief HTTP Gateway
 *
 * 负责将 HTTP 请求转换为 StackFlows RPC / Event。
 * 不包含任何推理、调度或 session 管理逻辑。
 * 1.一个 Gateway，多个 handler
 * 2.不保存状态
 * 3.不知道 unit / node / task
 */
class HttpGateway
{
public:
    // explicit HttpGateway(StackFlowsClient *client);
    explicit HttpGateway(StackFlowsClient *client, ModelEngine *engine);
    ~HttpGateway() = default;

    // 非流式 completion
    void HandleCompletion(const HttpRequest &req,
                          HttpResponse &res);

    // 流式 completion（SSE）
    void HandleCompletionStream(const HttpRequest &req,
                                std::shared_ptr<HttpResponse> res_ptr);

    // 新增：Chat
    void HandleChatCompletion(const HttpRequest &req, HttpResponse &res);
    void HandleChatCompletionStream(const HttpRequest &req,
                                    std::shared_ptr<HttpResponse> res_ptr);

private:
    StackFlowsClient *sf_client_;   // 不持有所有权
    ModelEngine *engine_;           // 不持有所有权
};
