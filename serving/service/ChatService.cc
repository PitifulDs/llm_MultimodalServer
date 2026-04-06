#include "serving/service/ChatService.h"

#include "serving/core/EngineExecutor.h"
#include "serving/core/Session.h"
#include "serving/core/SessionExecutor.h"
#include "serving/core/SessionManager.h"
#include "serving/core/ServingContext.h"
#include "serving/http/HttpUtils.h"
#include "serving/service/ModelCatalogService.h"

#include <algorithm>
#include <chrono>
#include <glog/logging.h>
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

    if (!model_catalog_service_.SupportsCapability(ctx->model, ctx->capability, ctx->inference_backend))
    {
        return {
            ChatErrorKind::InvalidRequest,
            "unsupported_capability",
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
        result.ctx->params["error_code"] = "overloaded";
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
        request.ctx->params["error_code"] = "overloaded";
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
    const bool overloaded =
        error_code == "overloaded" ||
        (ctx->error_message.find("queue full") != std::string::npos);

    if (error_code == "invalid_request" ||
        error_code == "model_not_found" ||
        error_code == "unsupported_capability")
    {
        return {
            ChatErrorKind::InvalidRequest,
            error_code,
            ctx->error_message.empty() ? "invalid chat request" : ctx->error_message};
    }

    if (overloaded)
    {
        return {
            ChatErrorKind::RateLimit,
            "queue_full",
            ctx->error_message.empty() ? "engine overloaded" : ctx->error_message};
    }

    return {
        ChatErrorKind::Internal,
        error_code.empty() ? "internal_error" : error_code,
        ctx->error_message.empty() ? "engine error" : ctx->error_message};
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
    const char *mt_val = nullptr;
    auto mt_it = ctx->params.find("max_tokens");
    if (mt_it != ctx->params.end())
        mt_val = mt_it->second.c_str();

    LOG(INFO) << (stream ? "[chat-stream] start req=" : "[chat] start req=") << ctx->request_id
              << " model=" << ctx->model
              << " session=" << ctx->session_id
              << " stream=" << (stream ? 1 : 0)
              << " agent=" << (ctx->use_agent ? 1 : 0)
              << " max_tokens=" << (mt_val ? mt_val : "default");
}

void ChatService::AttachNonStreamFinishHandler(const ChatExecutionRequest &request,
                                               std::chrono::steady_clock::time_point start_time) const
{
    auto ctx = request.ctx;
    const auto client_messages = request.client_messages;
    ctx->on_finish = [this, ctx, client_messages, start_time](FinishReason reason)
    {
        if (reason == FinishReason::stop || reason == FinishReason::length)
            UpdateSessionHistory(ChatExecutionRequest{ctx, client_messages});

        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        if (callbacks_.record_finish)
            callbacks_.record_finish(reason, dur_ms);

        LOG(INFO) << "[chat] done req=" << ctx->request_id
                  << " model=" << ctx->model
                  << " dur_ms=" << dur_ms
                  << " prompt_tokens=" << ctx->usage.prompt_tokens
                  << " completion_tokens=" << ctx->usage.completion_tokens
                  << " reason=" << http_utils::finish_reason_to_str(reason);
    };
}

void ChatService::AttachStreamFinishHandler(const ChatExecutionRequest &request,
                                            const std::function<void()> &close_stream,
                                            std::chrono::steady_clock::time_point start_time) const
{
    auto ctx = request.ctx;
    const auto client_messages = request.client_messages;
    ctx->on_finish = [this, ctx, client_messages, close_stream, start_time](FinishReason reason)
    {
        if (reason == FinishReason::stop || reason == FinishReason::length)
            UpdateSessionHistory(ChatExecutionRequest{ctx, client_messages});

        close_stream();

        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        if (callbacks_.record_finish)
            callbacks_.record_finish(reason, dur_ms);

        LOG(INFO) << "[chat-stream] done req=" << ctx->request_id
                  << " model=" << ctx->model
                  << " dur_ms=" << dur_ms
                  << " prompt_tokens=" << ctx->usage.prompt_tokens
                  << " completion_tokens=" << ctx->usage.completion_tokens
                  << " reason=" << http_utils::finish_reason_to_str(reason);
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
