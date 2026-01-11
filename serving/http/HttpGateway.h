#pragma once
#include <memory>
#include <string>
#include "protocol/Protocol.h"
#include "serving/core/SessionManager.h"
#include "serving/core/ModelEngine.h"

// 前向声明
struct HttpRequest;
struct HttpResponse;
class StackFlowsClient;
struct ServingContext;

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
    HttpGateway();
    ~HttpGateway() = default;

    // 非流式 completion
    void HandleCompletion(const HttpRequest &req, HttpResponse &res);

    // 流式 completion（SSE）
    void HandleCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr);

    // 新增：Chat
    void HandleChatCompletion(const HttpRequest &req, HttpResponse &res);
    void HandleChatCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr);

private:
    StackFlowsClient *sf_client_{nullptr};   // 不持有所有权
    std::unique_ptr<SessionManager> session_mgr_;
    std::shared_ptr<ModelEngine> engine_;
};
