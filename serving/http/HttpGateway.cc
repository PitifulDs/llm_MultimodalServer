#include "HttpGateway.h"

#include "http_types.h"
#include "HttpStreamSession.h"
#include "ChatRequestParser.h"
#include "serving/core/ServingContext.h"
#include "serving/core/SessionManager.h"
#include "OpenAIStreamWriter.h"
#include "HttpUtils.h"

#include "../../utils/json.hpp"
#include <glog/logging.h>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
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

    int get_default_max_tokens()
    {
        const char *env = std::getenv("DEFAULT_MAX_TOKENS");
        if (!env || !*env)
            return 0;
        try
        {
            int v = std::stoi(env);
            return v > 0 ? v : 0;
        }
        catch (...)
        {
            return 0;
        }
    }

    std::string gen_request_id()
    {
        static std::atomic<uint64_t> seq{0};
        return "req-" + std::to_string(++seq);
    }

    std::filesystem::path detect_repo_root()
    {
        if (const char *cfg = std::getenv("CONFIG_PATH"))
        {
            if (*cfg)
            {
                std::error_code ec;
                auto cfg_path = std::filesystem::weakly_canonical(std::filesystem::path(cfg), ec);
                if (ec)
                    cfg_path = std::filesystem::path(cfg).lexically_normal();
                if (cfg_path.has_parent_path())
                    return cfg_path.parent_path();
            }
        }
        return std::filesystem::current_path();
    }

    // FinishReason -> openai finish_reaso
    const char *finish_reason_to_str(FinishReason r)
    {
        return http_utils::finish_reason_to_str(r);
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

    AgentExecutor::Options agent_opt;
    const auto repo_root = detect_repo_root();
    agent_opt.repo_root = repo_root.string();
    agent_opt.docs_root = repo_root.string();
    agent_opt.config_path = (repo_root / "config.json").string();
    if (const char *cfg = std::getenv("CONFIG_PATH"))
    {
        if (*cfg)
            agent_opt.config_path = cfg;
    }
    agent_executor_ = std::make_unique<AgentExecutor>(executor_, agent_opt);
    agent_executor_->SetStatusProvider([this]()
    {
        const auto uptime_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time_)
                .count();

        json out = {
            {"status", "ok"},
            {"uptime_ms", uptime_ms},
            {"requests_total", total_requests_.load(std::memory_order_relaxed)},
            {"requests_in_flight", in_flight_.load(std::memory_order_relaxed)},
            {"requests_stream_total", stream_requests_.load(std::memory_order_relaxed)},
            {"requests_error_total", error_requests_.load(std::memory_order_relaxed)},
            {"requests_cancelled_total", cancelled_requests_.load(std::memory_order_relaxed)}};
        return out.dump();
    });

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

void HttpGateway::HandleModels(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    json items = json::array();
    const auto models = ModelRegistry::ListModels();
    const std::string default_model = ModelRegistry::GetDefaultModel();

    for (const auto &name : models)
    {
        items.push_back({
            {"id", name},
            {"object", "model"},
            {"owned_by", "edge-llm-serving"},
            {"default", name == default_model}
        });
    }

    json out = {
        {"object", "list"},
        {"data", items}
    };

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

    const std::string request_id = gen_request_id();
    auto parsed = ParseChatRequestBody(req.body, false, *session_mgr_, get_default_model(), get_default_max_tokens(), request_id);
    if (!parsed.ok)
    {
        WriteError(res, parsed.status, parsed.message, parsed.type, parsed.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    auto ctx = parsed.request.ctx;
    const std::string model = ctx->model;

    const char *mt_val = nullptr;
    auto mt_it = ctx->params.find("max_tokens");
    if (mt_it != ctx->params.end())
        mt_val = mt_it->second.c_str();
    LOG(INFO) << "[chat] start req=" << ctx->request_id
              << " model=" << ctx->model
              << " session=" << ctx->session_id
              << " stream=0"
              << " agent=" << (ctx->use_agent ? 1 : 0)
              << " max_tokens=" << (mt_val ? mt_val : "default");

    // 备份客户端全量 messages（用于更新 history）
    const std::vector<Message> client_messages = parsed.request.client_messages;

    auto session = ctx->session;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        LOG(INFO) << "[auto-diff] session=" << session->session_id
                  << " incoming=" << client_messages.size()
                  << " delta=" << ctx->messages.size()
                  << " hist=" << session->history.size();
    }

    // on_finish：仅 stop/length 更新 history，避免 cancelled/error 污染 session
    ctx->on_finish = [this, session, ctx, client_messages, start_time, mgr = session_mgr_.get()](FinishReason r)
    {
        if (r == FinishReason::stop || r == FinishReason::length)
        {
            std::vector<Message> history_snapshot;
            {
                std::lock_guard<std::mutex> lk(session->mu);
                session->history = client_messages;
                session->history.push_back({"assistant", ctx->final_text});
                session->touch();
                history_snapshot = session->history;
            }
            if (mgr)
            {
                mgr->PersistHistory(session->session_id, history_snapshot);
            }
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
                                             {
                                                 if (ctx->use_agent && agent_executor_)
                                                 {
                                                     agent_executor_->Run(ctx);
                                                     return;
                                                 }
                                                 executor_.Execute(ctx);
                                             });

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

    const std::string request_id = gen_request_id();
    auto parsed = ParseChatRequestBody(req.body, true, *session_mgr_, get_default_model(), get_default_max_tokens(), request_id);
    if (!parsed.ok)
    {
        WriteError(*res_ptr, parsed.status, parsed.message, parsed.type, parsed.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    auto ctx = parsed.request.ctx;
    const std::string model = ctx->model;

    const char *mt_val = nullptr;
    auto mt_it = ctx->params.find("max_tokens");
    if (mt_it != ctx->params.end())
        mt_val = mt_it->second.c_str();
    LOG(INFO) << "[chat-stream] start req=" << ctx->request_id
              << " model=" << ctx->model
              << " session=" << ctx->session_id
              << " stream=1"
              << " agent=" << (ctx->use_agent ? 1 : 0)
              << " max_tokens=" << (mt_val ? mt_val : "default");

    // 备份客户端全量 messages（用于更新 history）
    const std::vector<Message> client_messages = parsed.request.client_messages;

    auto session = ctx->session;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        LOG(INFO) << "[auto-diff] session=" << session->session_id
                  << " incoming=" << client_messages.size()
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
        writer->OnChunk(chunk);
    };

    // on_finish：仅 stop/length 更新 history；然后关闭 SSE
    ctx->on_finish = [this, session, ctx, client_messages, http_session, start_time, mgr = session_mgr_.get()](FinishReason r)
    {
        if (r == FinishReason::stop || r == FinishReason::length)
        {
            std::vector<Message> history_snapshot;
            {
                std::lock_guard<std::mutex> lk(session->mu);
                session->history = client_messages;
                session->history.push_back({"assistant", ctx->final_text});
                session->touch();
                history_snapshot = session->history;
            }
            if (mgr)
            {
                mgr->PersistHistory(session->session_id, history_snapshot);
            }
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
        if (ctx->use_agent && agent_executor_)
        {
            agent_executor_->Run(ctx);
            return;
        }
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
