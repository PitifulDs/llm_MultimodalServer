#include "HttpGateway.h"
#include "http_types.h"
#include "HttpStreamSession.h"
#include "StackFlowsClient.h"
#include "protocol/Protocol.h"
#include "engine/EngineFactory.h"
#include "serving/core/ServingContext.h"
#include "serving/core/ModelEngine.h"
#include "../../utils/json.hpp"
#include "OpenAIStreamWriter.h"

#include <glog/logging.h>
#include <random>
#include <ctime>
#include <string>
#include <memory>
using json = nlohmann::json;

namespace
{

    // 简单生成 request_id（示例）
    std::string gen_request_id()
    {
        static std::atomic<uint64_t> seq{0};
        return "req-" + std::to_string(++seq);
    }
} // namespace

HttpGateway::HttpGateway(StackFlowsClient *client, ModelEngine *engine)
    : sf_client_(client), engine_(engine)
{
}

void HttpGateway::HandleCompletion(const HttpRequest &req, HttpResponse &res)
{
    // 1) 解析 JSON
    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (...)
    {
        // res.SetHeader("Content-Type", "application/json");
        // res.Write(R"({"error":"invalid json"})");
        res.Write(R"({"error":{"message":"invalid json","type":"invalid_request_error"}})");
        return;
    }

    const std::string prompt = body.value("prompt", "");
    const std::string model = body.value("model", "unkown");
    // const std::string session_id = body.value("session_id", "");

    // 2) 构造 RPC 请求（非流式）
    RpcRequest rpc;
    rpc.version = "v1";
    rpc.request_id = gen_request_id();
    rpc.session_id = body.value("session_id", "");
    rpc.action = "completion";
    rpc.stream = false;
    rpc.payload["prompt"] = prompt;

    // 3) 调用 StackFlows
    RpcResponse resp = sf_client_->Call(rpc);

    // 4) 构造openai-style 回复
    json out;
    out["id"] = "cmpl-" + rpc.request_id;
    out["object"] = "text_completion";
    out["created"] = static_cast<int>(std::time(nullptr));
    out["model"] = model;

    json choices;
    choices["index"] = 0;

    std::string text;
    auto it = resp.result.find("text");
    if (it != resp.result.end())
    {
        text = it->second;
    }
    else
    {
        text.clear();
    }

    choices["text"] = text;
    choices["finish_reason"] = "stop";

    out["choices"] = json::array({choices});
   
    res.SetHeader("Content-Type", "application/json");
    res.Write(out.dump());
}

void HttpGateway::HandleCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr)

{
    res_ptr->SetHeader("Content-Type", "application/json");
    res_ptr->Write(R"({
        "error":{
            "message":"completion stream not supported in Step 4.4",
            "type":"not_implemented"
        }
    })");
}

void HttpGateway::HandleChatCompletion(const HttpRequest &req, HttpResponse &res)
{
    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (...)
    {
        res.Write(R"({"error":{"message":"invalid json","type":"invalid_request_error"}})");
        return;
    }

    const std::string model = body.value("model", "unknown");
    const auto &messages = body["messages"];

    if (!messages.is_array())
    {
        res.Write(R"({"error":{"message":"messages must be array","type":"invalid_request_error"}})");
        return;
    }

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = gen_request_id();
    ctx->model = model;
    ctx->stream = false;

    for (const auto &m : messages)
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    OpenAIStreamWriter writer(
        ctx->request_id,
        ctx->model,
        nullptr // non-stream 不写
    );
   
    ctx->on_chunk = [&](const StreamChunk& ch) {
        writer.Collect(ch);
    };

    // 使用 LLMEngine
    auto llm_engine = EngineFactory::Create(ctx->model);
    llm_engine->GenerateStream(*ctx);
    std::string result = writer.Result();

    // non-stream：从 ctx 取结果
    json out = {
        {"id", "chatcmpl-" + ctx->request_id},
        {"object", "chat.completion"},
        {"created", static_cast<int>(std::time(nullptr))},
        {"model", model},
        {"choices", {{
            {"index", 0}, 
            {"message", {
                {"role", "assistant"}, 
                {"content", result}
            }}, 
                {"finish_reason", "stop"}
            }}}
        };

    res.SetHeader("Content-Type", "application/json");
    res.Write(out.dump());
}

void HttpGateway::HandleChatCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr)
{
    LOG(INFO) << "[chat-stream] enter HandleChatCompletionStream";

    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (...)
    {
        res_ptr->Write(R"({"error":{"message":"invalid json","type":"invalid_request_error"}})");
        return;
    }

    const auto &messages = body["messages"];
    const std::string model = body.value("model", "unknown");
    if (!messages.is_array())
    {
        res_ptr->Write(R"({"error":{"message":"messages must be array","type":"invalid_request_error"}})");
        return;
    }

    // 1) 构造 ctx
    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = gen_request_id();
    ctx->model = model;
    ctx->stream = true;

    for (const auto &m : messages)
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    // 2) 用 session 托管 response 生命周期
    auto session = std::make_shared<HttpStreamSession>(ctx->request_id, res_ptr);
    session->Start();

    // 3) OpenAI Stream Writer（协议层）
    OpenAIStreamWriter writer(ctx->request_id, ctx->model, [&](const std::string& s){
        if(session->IsAlive()){
            session->Write(s);
        }else{
            ctx->cancelled = true;
        }
    });

    // 4) Engine → Writer
    ctx->on_chunk = [&](const StreamChunk &chunk)
    {
        writer.OnChunk(chunk);
    };

    // 5) 启动引擎
    auto llm_engine = EngineFactory::Create(ctx->model);
    llm_engine->GenerateStream(*ctx);
}
