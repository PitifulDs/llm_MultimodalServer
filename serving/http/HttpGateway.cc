#include "HttpGateway.h"

#include "http_types.h"
#include "HttpStreamSession.h"
#include "serving/core/ServingContext.h"
#include "serving/core/SessionManager.h"
#include "OpenAIStreamWriter.h"

#include "../../utils/json.hpp"
#include <glog/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace
{
    std::string gen_request_id()
    {
        static std::atomic<uint64_t> seq{0};
        return "req-" + std::to_string(++seq);
    }

    bool msg_equal(const Message &a, const Message &b)
    {
        return a.role == b.role && a.content == b.content;
    }

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

    std::vector<Message> diff_messages(const std::vector<Message> &history,
                                       const std::vector<Message> &incoming)
    {
        if (!is_prefix(history, incoming))
        {
            return incoming;
        }
        return std::vector<Message>(incoming.begin() + history.size(), incoming.end());
    }

    // FinishReason -> openai finish_reaso
    const char *finish_reason_to_str(FinishReason r)
    {
        switch (r)
        {
        case FinishReason::stop:
            return "stop";
        case FinishReason::length:
            return "length";
        case FinishReason::cancelled:
            return "cancelled";
        case FinishReason::error:
        default:
            return "error";
        }
    }

} // namespace

HttpGateway::HttpGateway()
{
    SessionManager::Options opt;
    opt.idle_ttl = std::chrono::minutes(30);
    opt.max_sessions = 1024;
    opt.gc_batch = 64;

    session_mgr_ = std::make_unique<SessionManager>(opt);

    // Session GC 后台线程
    std::thread([mgr = session_mgr_.get()]()
                {
                    while (true)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(60));
                        const size_t removed = mgr->gc();
                        if (removed > 0)
                        {
                            LOG(INFO) << "[session-gc] removed=" << removed
                                      << " remaining=" << mgr->size();
                        }
                    } })
        .detach();
}

void HttpGateway::HandleCompletion(const HttpRequest &req, HttpResponse &res)
{
    (void)req;

    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");

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
    res_ptr->SetHeader("Connection", "close");
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
        res.SetHeader("Connection", "close");
        res.Write(R"({"error":{"message":"invalid json","type":"invalid_request_error"}})");
        return;
    }

    const std::string model = body.value("model", "unknown");
    if (!body.contains("messages") || !body["messages"].is_array())
    {
        res.SetHeader("Content-Type", "application/json");
        res.SetHeader("Connection", "close");
        res.Write(R"({"error":{"message":"messages must be array","type":"invalid_request_error"}})");
        return;
    }

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = gen_request_id();
    ctx->model = model;
    ctx->stream = false;

    // session_id
    std::string session_id;
    if (body.contains("session_id") && body["session_id"].is_string())
        session_id = body["session_id"].get<std::string>();
    else
        session_id = ctx->request_id;

    ctx->session_id = session_id;
    ctx->session = session_mgr_->getOrCreate(session_id, model);

    // parse messages
    ctx->messages.clear();
    for (const auto &m : body["messages"])
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    // 备份客户端全量 messages（用于更新 history）
    const std::vector<Message> client_messages = ctx->messages;

    // auto-diff（只在锁内读写 session）
    auto session = ctx->session;
    {
        std::lock_guard<std::mutex> lk(session->mu);

        const std::vector<Message> &incoming = ctx->messages;
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
    }

    // 让 non-stream 即使未来 executor 异步也能正确返回：等待 on_finish
    std::mutex done_mu;
    std::condition_variable done_cv;
    bool done = false;
    FinishReason final_reason = FinishReason::stop;

    ctx->on_finish = [&, session, client_messages](FinishReason r)
    {
        final_reason = r;

        // 更新 session 历史（assistant 内容用 ctx->final_text）
        {
            std::lock_guard<std::mutex> lk(session->mu);
            session->history = client_messages;
            session->history.push_back({"assistant", ctx->final_text});
            session->touch();
        }

        {
            std::lock_guard<std::mutex> lk(done_mu);
            done = true;
        }
        done_cv.notify_one();
    };

    executor_.Execute(ctx);

    // 等完成（如果 executor 同步，这里会立刻 done；如果异步，这里阻塞到完成）
    {
        std::unique_lock<std::mutex> lk(done_mu);
        done_cv.wait(lk, [&]
                     { return done; });
    }

    // 如果引擎设置了 error_message，也可以按现有字段返回错误
    if (!ctx->error_message.empty() || final_reason == FinishReason::error)
    {
        // overloaded -> 429 or 503
        const bool overloaded = (ctx->params.count("error_code") && ctx->params["error_code"] == "overloaded") || (ctx->error_message.find("queue full") != std::string::npos);

        if (overloaded)
        {
            res.SetStatus(429, "Too Many Requests"); // 或 503
        }
        else
        {
            res.SetStatus(500, "Internal Server Error");
        }

        json err = {
            {"error", {{"message", ctx->error_message.empty() ? "engine error" : ctx->error_message}, {"type", overloaded ? "rate_limit_error" : "internal_error"}}}};

        res.SetHeader("Content-Type", "application/json");
        res.SetHeader("Connection", "close");
        res.Write(err.dump());
        return;
    }

    json out = {
        {"id", "chatcmpl-" + ctx->request_id},
        {"object", "chat.completion"},
        {"created", static_cast<int>(std::time(nullptr))},
        {"model", model},
        {"choices", {{{"index", 0}, {"message", {{"role", "assistant"}, {"content", ctx->final_text}}}, {"finish_reason", finish_reason_to_str(final_reason)}}}}};

    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
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
        res_ptr->SetStatus(400, "Bad Request");
        res_ptr->SetHeader("Content-Type", "application/json");
        res_ptr->SetHeader("Connection", "close");
        res_ptr->Write(R"({"error":{"message":"invalid json","type":"invalid_request_error"}})");
        return;
    }

    if (!body.contains("messages") || !body["messages"].is_array())
    {
        res_ptr->SetStatus(400, "Bad Request");
        res_ptr->SetHeader("Content-Type", "application/json");
        res_ptr->SetHeader("Connection", "close");
        res_ptr->Write(R"({"error":{"message":"messages must be array","type":"invalid_request_error"}})");
        return;
    }

    const std::string model = body.value("model", "unknown");

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = gen_request_id();
    ctx->model = model;
    ctx->stream = true;

    std::string session_id;
    if (body.contains("session_id") && body["session_id"].is_string())
        session_id = body["session_id"].get<std::string>();
    else
        session_id = ctx->request_id;

    ctx->session_id = session_id;
    ctx->session = session_mgr_->getOrCreate(session_id, model);

    // parse messages
    ctx->messages.clear();
    for (const auto &m : body["messages"])
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }

    // 备份客户端全量 messages（用于更新 history）
    const std::vector<Message> client_messages = ctx->messages;

    // auto-diff（锁内只处理 session 状态，锁外执行 engine）
    auto session = ctx->session;
    {
        std::lock_guard<std::mutex> lk(session->mu);

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
    }

    // 绑定 HttpStreamSession 生命周期（先不 Start）
    auto http_session = std::make_shared<HttpStreamSession>(ctx->request_id, res_ptr);

    // writer：将 OpenAI chunk -> SSE string -> session->Write
    auto writer = std::make_shared<OpenAIStreamWriter>(
        ctx->request_id, ctx->model,
        [http_session, ctx](const std::string &s)
        {
            if (!http_session->IsAlive())
            {
                ctx->cancelled.store(true);
                return;
            }

            http_session->Write(s);

            if (!http_session->IsAlive())
            {
                ctx->cancelled.store(true);
            }
        });

    // on_chunk：拼接 final_text + 喂给 writer
    ctx->on_chunk = [writer, ctx](const StreamChunk &chunk)
    {
        if (!chunk.is_finished)
        {
            ctx->final_text += chunk.delta;
        }
        writer->OnChunk(chunk);
    };

    // on_finish：仅 stop/length 更新 history；然后关闭 SSE
    ctx->on_finish = [session, ctx, client_messages, http_session](FinishReason r)
    {
        if (r == FinishReason::stop || r == FinishReason::length)
        {
            std::lock_guard<std::mutex> lk(session->mu);
            session->history = client_messages;
            session->history.push_back({"assistant", ctx->final_text});
            session->touch();
        }
        http_session->Close();
    };

    // 只 Execute 一次：先入队判断是否被接收
    bool accepted = executor_.Execute(ctx);
    if (!accepted)
    {
        const bool overloaded =
            (ctx->params.count("error_code") && ctx->params["error_code"] == "overloaded") ||
            (ctx->error_message.find("queue full") != std::string::npos);

        res_ptr->SetStatus(overloaded ? 429 : 500,
                           overloaded ? "Too Many Requests" : "Internal Server Error");
        res_ptr->SetHeader("Content-Type", "application/json");
        res_ptr->SetHeader("Connection", "close");

        json err = {{"error",
                     {{"message", ctx->error_message.empty() ? "engine error" : ctx->error_message},
                      {"type", overloaded ? "rate_limit_error" : "internal_error"}}}};
        res_ptr->Write(err.dump());
        return;
    }

    // accepted 后再发 SSE 头（避免队列满却先发 200 event-stream）
    http_session->Start();
}
