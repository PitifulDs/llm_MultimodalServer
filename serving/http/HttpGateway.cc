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
#include "serving/core/SessionManager.h"

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

    // auto-diff: 比较 message 是否相同
    bool msg_equal(const Message &a, const Message &b)
    {
        return a.role == b.role && a.content == b.content;
    }

    // auto-diff: 判断 history 是否是 incoming 的前缀
    bool is_prefix(const std::vector<Message> &history,
                          const std::vector<Message> &incoming)
    {
        if (history.size() > incoming.size())
            return false;
        for (size_t i = 0; i < history.size(); ++i)
        {
            if (!msg_equal(history[i], incoming[i]))
                return false;
        }
        return true;
    }

    // auto-diff: 计算增量 messages
    std::vector<Message> diff_messages(const std::vector<Message> &history,
                                              const std::vector<Message> &incoming)
    {
        if (!is_prefix(history, incoming))
        {
            return incoming; // caller 决定是否 reset
        }
        return std::vector<Message>(incoming.begin() + history.size(), incoming.end());
    }

} // namespace

HttpGateway::HttpGateway()
{
    SessionManager::Options opt;
    opt.idle_ttl = std::chrono::minutes(30);
    opt.max_sessions = 1024;

    session_mgr_ = std::make_unique<SessionManager>(opt);
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
        res.SetHeader("Content-Type", "application/json");
        res.Write(R"({"error":{"message":"invalid json","type":"invalid_request_error"}})");
        return;
    }

    const std::string model = body.value("model", "unknown");
    const auto &messages = body["messages"];

    if (!messages.is_array())
    {
        res.SetHeader("Content-Type", "application/json");
        res.Write(R"({"error":{"message":"messages must be array","type":"invalid_request_error"}})");
        return;
    }

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = gen_request_id();
    ctx->model = model;
    ctx->stream = false;

    // 解析session_id
    std::string session_id;
    if(body.contains("session_id") && body["session_id"].is_string())
    {
        session_id = body["session_id"].get<std::string>();
    }else{
        session_id = ctx->request_id;
    }

    // ctx->session_id = session_id;
    ctx->session = session_mgr_->getOrCreate(session_id, model);

    for (const auto &m : messages)
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    for (const auto &m : messages)
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    // 使用 LLMEngine
    auto& session = ctx->session;
    std::unique_lock<std::mutex> lk(session->mu);

    // auto - diff : incoming = 客户端传来的全量 messages
    const std::vector<Message>& incoming = ctx->messages;

    // 如果 session 已有历史，尝试做前缀 diff
    if (!session->history.empty())
    {
        if (is_prefix(session->history, incoming))
        {
            // 只取增量（避免重复 tokenize/append）
            ctx->messages = diff_messages(session->history, incoming);
        }
        else
        {
            // 不匹配：保守起见 reset session（避免 KV / 历史串乱）
            session->history.clear();
            session->model_ctx.reset(); // 释放 KV（ModelContext 析构会 free）
            ctx->messages = incoming;   // 当作首轮
        }
    }
    else
    {
        // 首轮：直接用 incoming
        ctx->messages = incoming;
    }

    LOG(INFO) << "[auto-diff] session=" << session->session_id
              << " incoming=" << incoming.size()
              << " delta=" << ctx->messages.size()
              << " hist=" << session->history.size();

    auto llm_engine = EngineFactory::Create(ctx->model);
    llm_engine->Run(ctx);

    // non-stream：从 ctx 取结果
    json out = {
        {"id", "chatcmpl-" + ctx->request_id},
        {"object", "chat.completion"},
        {"created", static_cast<int>(std::time(nullptr))},
        {"model", model},
        {"choices", {{{"index", 0}, {"message", {{"role", "assistant"}, {"content", ctx->final_text}}}, {"finish_reason", "stop"}}}}};

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

    // 解析session_id
    std::string session_id;
    if (body.contains("session_id") && body["session_id"].is_string())
    {
        session_id = body["session_id"].get<std::string>();
    }
    else
    {
        session_id = ctx->request_id;
    }

    ctx->session_id = session_id;
    ctx->session = session_mgr_->getOrCreate(session_id, model);

    for (const auto &m : messages)
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    // 2) 用 session 托管 response 生命周期
    auto http_session = std::make_shared<HttpStreamSession>(ctx->request_id, res_ptr);
    http_session->Start();

    // 3) OpenAI Stream Writer（协议层）
    OpenAIStreamWriter writer(ctx->request_id, ctx->model, [&](const std::string &s)
                              {
        if(http_session->IsAlive()){
            http_session->Write(s);
        }else{
            ctx->cancelled = true;
        } });

    // 4) Engine → Writer
    ctx->on_chunk = [&](const StreamChunk &chunk)
    {
        writer.OnChunk(chunk);
    };

    // 5) 启动引擎
    auto &session = ctx->session;
    std::unique_lock<std::mutex> lk(session->mu);

    // auto-diff
    const std::vector<Message> incoming = ctx->messages;

    if (!session->history.empty())
    {
        if (is_prefix(session->history, incoming))
        {
            ctx->messages = diff_messages(session->history, incoming);
        }
        else
        {
            session->history.clear();
            session->model_ctx.reset();
            ctx->messages = incoming;
        }
    }
    else
    {
        ctx->messages = incoming;
    }

    LOG(INFO) << "[auto-diff] session=" << session->session_id
              << " incoming=" << incoming.size()
              << " delta=" << ctx->messages.size()
              << " hist=" << session->history.size();

    auto llm_engine = EngineFactory::Create(ctx->model);
    llm_engine->Run(ctx);
}
