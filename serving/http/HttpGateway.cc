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
#include <thread>
#include <chrono>

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
    opt.gc_batch = 64;

    session_mgr_ = std::make_unique<SessionManager>(opt);

    // 1) 创建唯一 Engine
    engine_ = EngineFactory::Create("llama");

    // 2) warmup（同步，一次即可）
    LOG(INFO) << "[serving-http] warming up model...";
    auto warmup_ctx = std::make_shared<ServingContext>();
    warmup_ctx->model = "llama";
    warmup_ctx->stream = false;
    warmup_ctx->messages = {
        {"user", "Hello"}};
    engine_->Run(warmup_ctx);
    LOG(INFO) << "[serving-http] warmup done";

    // 3) Session GC 后台线程
    std::thread([mgr = session_mgr_.get()]()
                {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            const size_t removed = mgr->gc();
            if (removed > 0) {
                LOG(INFO) << "[session-gc] removed=" << removed
                          << " remaining=" << mgr->size();
            }
        } })
        .detach();
}

void HttpGateway::HandleCompletion(const HttpRequest &req, HttpResponse &res)
{
    (void)req; // 明确表示不使用请求参数

    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");

    // 3. 构建规范的 OpenAI 风格错误响应
    res.Write(R"({
        "error": {
            "message": "The /v1/completions endpoint is deprecated in Serving v2. Please use /v1/chat/completions instead.",
            "type": "invalid_request_error",
            "param": null,
            "code": "endpoint_deprecated"
        }
    })");
}

void HttpGateway::HandleCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr)
{
    (void)req;
    res_ptr->SetHeader("Content-Type", "application/json");
    res_ptr->Write(R"({"error":{"message":"completion stream not supported","type":"not_implemented"}})");
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

    ctx->session_id = session_id;
    ctx->session = session_mgr_->getOrCreate(session_id, model);

    // parse messages（只 push 一遍）
    ctx->messages.clear();
    for (const auto &m : messages)
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    // 保存客户端“全量 messages”，后面要用来写 session->history
    // 因为 ctx->messages 会被 auto-diff 改成增量
    const std::vector<Message> client_messages = ctx->messages;

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

    engine_->Run(ctx);

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

    ctx->messages.clear();
    for (const auto &m : messages)
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    // 保存客户端全量 messages（因为 ctx->messages 会被 auto-diff 改成增量）
    const std::vector<Message> client_messages = ctx->messages;

    // 2) 用 session 托管 response 生命周期
    auto http_session = std::make_shared<HttpStreamSession>(ctx->request_id, res_ptr);
    http_session->Start();

    // stream 模式下累计 assistant 文本，用于写 session->history
    auto assistant_acc = std::make_shared<std::string>();

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
        // [NEW] 累计内容（stream 模式下 ServingContext::EmitDelta 不会填 final_text） if (!chunk.is_finished)
        {
            assistant_acc->append(chunk.delta);
        }
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

    engine_->Run(ctx);

    // 更新 session 历史（客户端全量 + 本次生成 assistant）
    session->history = client_messages;
    session->history.push_back({"assistant", *assistant_acc});
    session->touch();
}
