#include "HttpGateway.h"

#include "http_types.h"
#include "HttpStreamSession.h"
#include "serving/core/ServingContext.h"
#include "serving/core/SessionManager.h"
#include "OpenAIStreamWriter.h"

#include "../../utils/json.hpp"
#include <glog/logging.h>

#include <atomic>
#include <cstdlib>
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
    size_t get_worker_threads()
    {
        const char *env = std::getenv("WORKER_THREADS");
        if (!env || !*env)
            return 4;
        try
        {
            int v = std::stoi(env);
            return v > 0 ? static_cast<size_t>(v) : 4;
        }
        catch (...)
        {
            return 4;
        }
    }

    std::string get_default_model()
    {
        const char *env = std::getenv("DEFAULT_MODEL");
        if (env && *env)
            return std::string(env);
        return "llama";
    }

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
    : pool_(get_worker_threads()),
      executor_(pool_),
      session_executor_(pool_),
      start_time_(std::chrono::steady_clock::now())
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
        } 
    }).detach();
}

void HttpGateway::WriteError(HttpResponse &res, int status, const std::string &message,
                             const std::string &type, const std::string &code,
                             const std::string &param)
{
    res.SetStatus(status);
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");

    json err = {
        {"error",
         {{"message", message},
          {"type", type}}}};

    if (!code.empty())
        err["error"]["code"] = code;
    if (!param.empty())
        err["error"]["param"] = param;

    res.Write(err.dump(-1, ' ', false, json::error_handler_t::replace));
    res.End();
}

void HttpGateway::RecordFinish(FinishReason reason, int64_t dur_ms)
{
    total_latency_ms_.fetch_add(dur_ms, std::memory_order_relaxed);
    in_flight_.fetch_sub(1, std::memory_order_relaxed);

    if (reason == FinishReason::error)
        error_requests_.fetch_add(1, std::memory_order_relaxed);
    else if (reason == FinishReason::cancelled)
        cancelled_requests_.fetch_add(1, std::memory_order_relaxed);
}

void HttpGateway::HandleHealth(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const auto uptime_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_)
            .count();

    json out = {
        {"status", "ok"},
        {"uptime_ms", uptime_ms}};

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}

void HttpGateway::HandleMetrics(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const int64_t total = total_requests_.load(std::memory_order_relaxed);
    const int64_t latency = total_latency_ms_.load(std::memory_order_relaxed);
    const double avg_latency_ms = total > 0 ? static_cast<double>(latency) / static_cast<double>(total) : 0.0;

    json out = {
        {"requests_total", total},
        {"requests_in_flight", in_flight_.load(std::memory_order_relaxed)},
        {"requests_stream_total", stream_requests_.load(std::memory_order_relaxed)},
        {"requests_error_total", error_requests_.load(std::memory_order_relaxed)},
        {"requests_cancelled_total", cancelled_requests_.load(std::memory_order_relaxed)},
        {"avg_latency_ms", avg_latency_ms}};

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}

void HttpGateway::HandleCompletion(const HttpRequest &req, HttpResponse &res)
{
    (void)req;

    WriteError(res,
               400,
               "The /v1/completions endpoint is deprecated in Serving v2. Please use /v1/chat/completions instead.",
               "invalid_request_error",
               "endpoint_deprecated");
}

void HttpGateway::HandleCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr)
{
    (void)req;
    WriteError(*res_ptr, 501, "completion stream not supported", "not_implemented");
}

void HttpGateway::HandleChatCompletion(const HttpRequest &req, HttpResponse &res)
{
    const auto start_time = std::chrono::steady_clock::now();
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    in_flight_.fetch_add(1, std::memory_order_relaxed);

    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (...)
    {
        WriteError(res, 400, "invalid json", "invalid_request_error", "invalid_json");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const std::string model = body.value("model", get_default_model());
    if (!body.contains("messages") || !body["messages"].is_array())
    {
        WriteError(res, 400, "messages must be array", "invalid_request_error", "invalid_messages");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = gen_request_id();
    ctx->model = model;
    ctx->stream = false;
    ctx->is_chat = true;

    // session_id
    std::string session_id;
    if (body.contains("session_id") && body["session_id"].is_string())
        session_id = body["session_id"].get<std::string>();
    else
        session_id = ctx->request_id;

    ctx->session_id = session_id;
    ctx->session = session_mgr_->getOrCreate(session_id, model);

    // generation params
    if (body.contains("max_tokens") && body["max_tokens"].is_number_integer())
    {
        const int max_tokens = body["max_tokens"].get<int>();
        if (max_tokens > 0)
            ctx->params["max_tokens"] = std::to_string(max_tokens);
    }

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

    // on_finish：仅 stop/length 更新 history，避免 cancelled/error 污染 session
    ctx->on_finish = [this, session, ctx, client_messages, start_time](FinishReason r)
    {
        if (r == FinishReason::stop || r == FinishReason::length)
        {
            std::lock_guard<std::mutex> lk(session->mu);
            session->history = client_messages;
            session->history.push_back({"assistant", ctx->final_text});
            session->touch();
        }

        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(r, dur_ms);
        LOG(INFO) << "[chat] done req=" << ctx->request_id
                  << " model=" << ctx->model
                  << " dur_ms=" << dur_ms
                  << " prompt_tokens=" << ctx->usage.prompt_tokens
                  << " completion_tokens=" << ctx->usage.completion_tokens
                  << " reason=" << finish_reason_to_str(r);
    };

    // non-stream：连接断开立即取消，唤醒等待
    res.SetOnClose([ctx]
                   {
                       ctx->cancelled.store(true, std::memory_order_release);
                       ctx->EmitFinish(FinishReason::cancelled); });

    // 同 session 串行执行（只 Execute 一次）
    bool accepted = session_executor_.Submit(ctx->session, [this, ctx]
                                             { executor_.Execute(ctx); });

    if (!accepted)
    {
        ctx->error_message = "SessionExecutor: session queue full, session=" + ctx->session_id;
        ctx->params["error_code"] = "overloaded";
        ctx->EmitFinish(FinishReason::error);
    }

    // 等待完成 + 断连取消（res.IsAlive() == false 时自动 cancelled + EmitFinish）
    ctx->WaitFinishOrCancel([&res]
                            { return res.IsAlive(); }, std::chrono::milliseconds(100));

    // 客户端已断开：无需再回包（避免写死 socket / 无意义日志）
    if (!res.IsAlive())
    {
        return;
    }

    const FinishReason final_reason = ctx->finish_reason;

    // 错误返回（包含 overloaded）
    if (!ctx->error_message.empty() || final_reason == FinishReason::error)
    {
        const bool overloaded =
            (ctx->params.count("error_code") && ctx->params["error_code"] == "overloaded") ||
            (ctx->error_message.find("queue full") != std::string::npos);

        WriteError(res,
                   overloaded ? 429 : 500,
                   ctx->error_message.empty() ? "engine error" : ctx->error_message,
                   overloaded ? "rate_limit_error" : "internal_error",
                   overloaded ? "queue_full" : "internal_error");
        return;
    }

    // 正常返回
    json out = {
        {"id", "chatcmpl-" + ctx->request_id},
        {"object", "chat.completion"},
        {"created", static_cast<int>(std::time(nullptr))},
        {"model", model},
        {"choices",
         {{{"index", 0},
           {"message", {{"role", "assistant"}, {"content", ctx->final_text}}},
           {"logprobs", nullptr},
           {"finish_reason", finish_reason_to_str(final_reason)}}}},
        {"usage",
         {{"prompt_tokens", ctx->usage.prompt_tokens},
          {"completion_tokens", ctx->usage.completion_tokens},
          {"total_tokens", ctx->usage.total_tokens}
         }
        }
    };

    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump(-1, ' ', false, json::error_handler_t::replace));
    res.End();
}

void HttpGateway::HandleChatCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr)
{
    const auto start_time = std::chrono::steady_clock::now();
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    stream_requests_.fetch_add(1, std::memory_order_relaxed);
    in_flight_.fetch_add(1, std::memory_order_relaxed);

    LOG(INFO) << "[chat-stream] enter HandleChatCompletionStream";

    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (...)
    {
        WriteError(*res_ptr, 400, "invalid json", "invalid_request_error", "invalid_json");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    if (!body.contains("messages") || !body["messages"].is_array())
    {
        WriteError(*res_ptr, 400, "messages must be array", "invalid_request_error", "invalid_messages");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const std::string model = body.value("model", get_default_model());

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = gen_request_id();
    ctx->model = model;
    ctx->stream = true;
    ctx->is_chat = true;

    std::string session_id;
    if (body.contains("session_id") && body["session_id"].is_string())
        session_id = body["session_id"].get<std::string>();
    else
        session_id = ctx->request_id;

    ctx->session_id = session_id;
    ctx->session = session_mgr_->getOrCreate(session_id, model);

    // generation params
    if (body.contains("max_tokens") && body["max_tokens"].is_number_integer())
    {
        const int max_tokens = body["max_tokens"].get<int>();
        if (max_tokens > 0)
            ctx->params["max_tokens"] = std::to_string(max_tokens);
    }

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
    res_ptr->SetOnClose([ctx, http_session]
                        {
                            ctx->cancelled.store(true);
                            http_session->Close();
                        });

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
    ctx->on_finish = [this, session, ctx, client_messages, http_session, start_time](FinishReason r)
    {
        if (r == FinishReason::stop || r == FinishReason::length)
        {
            std::lock_guard<std::mutex> lk(session->mu);
            session->history = client_messages;
            session->history.push_back({"assistant", ctx->final_text});
            session->touch();
        }
        http_session->Close();

        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(r, dur_ms);
        LOG(INFO) << "[chat-stream] done req=" << ctx->request_id
                  << " model=" << ctx->model
                  << " dur_ms=" << dur_ms
                  << " prompt_tokens=" << ctx->usage.prompt_tokens
                  << " completion_tokens=" << ctx->usage.completion_tokens
                  << " reason=" << finish_reason_to_str(r);
    };

    // accepted 后再发 SSE 头（避免队列满却先发 200 event-stream）
    http_session->Start();

    // 同 session 串行执行（只 Execute 一次）
    bool accepted  = session_executor_.Submit(session, [this, ctx]
    {
        executor_.Execute(ctx);
        // executor 内部会在 queue full 时 EmitFinish(error)，writer 会输出对应 SSE 并结束
    });

    if (!accepted)
    {
        ctx->error_message = "SessionExecutor: session queue full, session=" + ctx->session_id;
        ctx->params["error_code"] = "overloaded";
        ctx->EmitFinish(FinishReason::error);
    }
}
