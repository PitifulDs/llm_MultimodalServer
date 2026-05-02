#include "serving/service/ChatService.h"

#include "serving/core/EngineExecutor.h"
#include "serving/core/Session.h"
#include "serving/core/SessionExecutor.h"
#include "serving/core/SessionManager.h"
#include "serving/core/ServingContext.h"
#include "serving/http/HttpUtils.h"
#include "serving/service/ModelCatalogService.h"
#include "serving/service/RequestLogging.h"

#include <algorithm>
#include <chrono>
#include <glog/logging.h>
#include <cstdlib>
#include <mutex>

namespace
{
void prepare_session_delta(const std::shared_ptr<ServingContext> &ctx,
                           const std::vector<Message> &incoming)
{
    if (!ctx || !ctx->session)
        return;

    auto session = ctx->session;
    std::lock_guard<std::mutex> lk(session->mu);
    if (!session->history.empty())
    {
        if (http_utils::is_prefix(session->history, incoming))
        {
            ctx->messages = http_utils::diff_messages(session->history, incoming);
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

int64_t get_param_int64(const std::shared_ptr<ServingContext> &ctx,
                        const std::string &key,
                        int64_t default_value = 0)
{
    if (!ctx)
        return default_value;

    const auto it = ctx->params.find(key);
    if (it == ctx->params.end() || it->second.empty())
        return default_value;

    try
    {
        return std::stoll(it->second);
    }
    catch (...)
    {
        return default_value;
    }
}
} // namespace

ChatService::ChatService(SessionExecutor &session_executor,
                         EngineExecutor &engine_executor,
                         ModelCatalogService &model_catalog_service,
                         SessionManager &session_manager,
                         ExtensionHooks extension_hooks,
                         Callbacks callbacks)
    : session_executor_(session_executor),
      engine_executor_(engine_executor),
      model_catalog_service_(model_catalog_service),
      session_manager_(session_manager),
      extension_hooks_(std::move(extension_hooks)),
      callbacks_(std::move(callbacks))
{
}

ChatError ChatService::ValidateRequest(const ChatExecutionRequest &request) const
{
    if (!request.ctx)
    {
        return {ChatErrorKind::Internal, "chat_context_missing", "chat context unavailable"};
    }

    const auto &ctx = request.ctx;
    if (!model_catalog_service_.HasModel(ctx->model))
    {
        return {
            ChatErrorKind::InvalidRequest,
            "model_not_found",
            "model not found: " + ctx->model};
    }

    if (!ctx->inference_backend.empty())
    {
        const ModelSpec spec = model_catalog_service_.ResolveModel(ctx->model,
                                                                   ctx->capability,
                                                                   ctx->inference_backend);
        if (!spec.valid)
        {
            return {
                ChatErrorKind::InvalidRequest,
                "backend_not_available",
                "requested backend is not declared or does not support capability for model: " + ctx->model};
        }
    }

    if (!model_catalog_service_.SupportsCapability(ctx->model, ctx->capability, ctx->inference_backend))
    {
        return {
            ChatErrorKind::InvalidRequest,
            "capability_not_supported",
            "model does not support capability: " + std::string(ToString(ctx->capability))};
    }

    return {};
}

ChatService::NonStreamResult ChatService::RunNonStream(const ChatExecutionRequest &request,
                                                       const std::function<void(std::function<void()>)> &bind_on_close,
                                                       const std::function<bool()> &is_client_alive,
                                                       std::chrono::steady_clock::time_point start_time) const
{
    NonStreamResult result;
    result.ctx = request.ctx;
    if (!result.ctx)
        return result;

    LogChatStart(result.ctx, false);
    AttachNonStreamFinishHandler(request, start_time);

    bind_on_close([ctx = result.ctx]
                  {
                      ctx->cancelled.store(true, std::memory_order_release);
                      ctx->EmitFinish(FinishReason::cancelled);
                  });

    if (!SubmitChatExecution(request))
    {
        result.ctx->error_message = "SessionExecutor: session queue full, session=" + result.ctx->session_id;
        result.ctx->params["error_code"] = "queue_full";
        result.ctx->EmitFinish(FinishReason::error);
    }

    result.ctx->WaitFinishOrCancel(is_client_alive, std::chrono::milliseconds(100));
    result.client_closed = !is_client_alive();
    return result;
}

void ChatService::RunStream(const ChatExecutionRequest &request,
                            const std::function<void(std::function<void()>)> &bind_on_close,
                            const std::function<void()> &start_stream,
                            const std::function<void()> &close_stream,
                            std::chrono::steady_clock::time_point start_time) const
{
    if (!request.ctx)
        return;

    LogChatStart(request.ctx, true);
    AttachStreamFinishHandler(request, close_stream, start_time);

    bind_on_close([ctx = request.ctx, close_stream]
                  {
                      ctx->cancelled.store(true, std::memory_order_release);
                      close_stream();
                  });

    start_stream();

    if (!SubmitChatExecution(request))
    {
        request.ctx->error_message = "SessionExecutor: session queue full, session=" + request.ctx->session_id;
        request.ctx->params["error_code"] = "queue_full";
        request.ctx->EmitFinish(FinishReason::error);
    }
}

ChatError ChatService::BuildError(const std::shared_ptr<ServingContext> &ctx) const
{
    if (!ctx)
    {
        return {ChatErrorKind::Internal, "chat_context_missing", "chat context unavailable"};
    }

    if (ctx->error_message.empty() && ctx->finish_reason != FinishReason::error)
        return {};

    ChatError extension_error;
    if (extension_hooks_.build_error && extension_hooks_.build_error(ctx, extension_error))
        return extension_error;

    const std::string error_code = ctx->params.count("error_code") ? ctx->params.at("error_code") : std::string();
    if (IsPlatformRateLimitCode(error_code) ||
        ctx->error_message.find("queue full") != std::string::npos)
    {
        return {
            ChatErrorKind::RateLimit,
            error_code.empty() ? "queue_full" : error_code,
            ctx->error_message.empty() ? "engine overloaded" : ctx->error_message};
    }

    return BuildPlatformErrorFromCode(error_code,
                                      ctx->error_message,
                                      "invalid chat request",
                                      "chat request cancelled",
                                      "request timed out",
                                      "backend unavailable",
                                      "engine overloaded",
                                      "engine error");
}

ChatResponse ChatService::BuildResponse(const std::shared_ptr<ServingContext> &ctx) const
{
    ChatResponse response;
    if (!ctx)
        return response;

    response.model = ctx->model;
    response.output_text = ctx->final_text;
    response.finish_reason = ctx->finish_reason;
    response.error_message = ctx->error_message;
    response.usage = ctx->usage;

    if (extension_hooks_.enrich_response)
        extension_hooks_.enrich_response(ctx, response);
    return response;
}

void ChatService::LogChatStart(const std::shared_ptr<ServingContext> &ctx, bool stream) const
{
    LogPlatformRequest("start", RequestLogRecord{
                                    ctx ? ctx->request_id : std::string(),
                                    "/v1/chat/completions",
                                    ctx ? ctx->model : std::string(),
                                    ctx ? ctx->inference_backend : std::string(),
                                    ctx ? std::string(ToString(ctx->capability)) : std::string(),
                                    ctx ? ctx->session_id : std::string(),
                                    -1,
                                    -1,
                                    "",
                                    0,
                                    "",
                                    0,
                                    0,
                                    0,
                                    stream});
}

void ChatService::AttachNonStreamFinishHandler(const ChatExecutionRequest &request,
                                               std::chrono::steady_clock::time_point start_time) const
{
    auto ctx = request.ctx;
    const auto client_messages = request.client_messages;
    std::weak_ptr<ServingContext> weak_ctx = ctx;
    ctx->on_finish = [this, weak_ctx, client_messages, start_time](FinishReason reason)
    {
        auto ctx = weak_ctx.lock();
        if (!ctx)
            return;

        if (reason == FinishReason::stop || reason == FinishReason::length)
            UpdateSessionHistory(ChatExecutionRequest{ctx, client_messages});

        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        const int64_t queue_wait_ms = get_param_int64(ctx, "queue_wait_ms", 0);
        if (callbacks_.record_finish)
            callbacks_.record_finish(reason, dur_ms);
        if (callbacks_.record_governance)
            callbacks_.record_governance(*ctx, reason, std::max<int64_t>(0, dur_ms - queue_wait_ms));
        ctx->request_state.reset();
    };
}

void ChatService::AttachStreamFinishHandler(const ChatExecutionRequest &request,
                                            const std::function<void()> &close_stream,
                                            std::chrono::steady_clock::time_point start_time) const
{
    auto ctx = request.ctx;
    const auto client_messages = request.client_messages;
    std::weak_ptr<ServingContext> weak_ctx = ctx;
    ctx->on_finish = [this, weak_ctx, client_messages, close_stream, start_time](FinishReason reason)
    {
        auto ctx = weak_ctx.lock();
        if (!ctx)
            return;

        if (reason == FinishReason::stop || reason == FinishReason::length)
            UpdateSessionHistory(ChatExecutionRequest{ctx, client_messages});

        close_stream();

        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        const int64_t queue_wait_ms = get_param_int64(ctx, "queue_wait_ms", 0);
        if (callbacks_.record_finish)
            callbacks_.record_finish(reason, dur_ms);
        if (callbacks_.record_governance)
            callbacks_.record_governance(*ctx, reason, std::max<int64_t>(0, dur_ms - queue_wait_ms));
        ctx->request_state.reset();
    };
}

bool ChatService::SubmitChatExecution(const ChatExecutionRequest &request) const
{
    auto ctx = request.ctx;
    const auto client_messages = request.client_messages;
    return session_executor_.Submit(ctx->session, [this, ctx, client_messages]
                                    {
                                        prepare_session_delta(ctx, client_messages);

                                        if (extension_hooks_.prepare_context &&
                                            !extension_hooks_.prepare_context(ctx))
                                        {
                                            if (!ctx->finished.load(std::memory_order_acquire))
                                                ctx->EmitFinish(FinishReason::error);
                                            return;
                                        }

                                        if (ctx->rag_options.enabled && callbacks_.record_rag_metrics)
                                            callbacks_.record_rag_metrics(*ctx);

                                        if (extension_hooks_.execute_request &&
                                            extension_hooks_.execute_request(ctx))
                                        {
                                            if (!ctx->finished.load(std::memory_order_acquire) &&
                                                ctx->cancelled.load(std::memory_order_acquire))
                                            {
                                                ctx->EmitFinish(FinishReason::cancelled);
                                            }
                                            return;
                                        }

                                        engine_executor_.Execute(ctx);
                                    });
}

void ChatService::UpdateSessionHistory(const ChatExecutionRequest &request) const
{
    if (!request.ctx || !request.ctx->session)
        return;

    std::vector<Message> history_snapshot;
    auto session = request.ctx->session;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        session->history = request.client_messages;
        session->history.push_back({"assistant", request.ctx->final_text});
        session->touch();
        history_snapshot = session->history;
    }
    session_manager_.PersistHistory(session->session_id, history_snapshot);
}
