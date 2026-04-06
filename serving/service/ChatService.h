#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "serving/service/ChatTypes.h"

class EngineExecutor;
class ModelCatalogService;
class SessionExecutor;
class SessionManager;

class ChatService
{
public:
    struct ExtensionHooks
    {
        std::function<bool(const std::shared_ptr<ServingContext> &)> prepare_context;
        std::function<bool(const std::shared_ptr<ServingContext> &)> execute_request;
        std::function<bool(const std::shared_ptr<ServingContext> &, ChatError &)> build_error;
        std::function<void(const std::shared_ptr<ServingContext> &, ChatResponse &)> enrich_response;
    };

    struct Callbacks
    {
        std::function<void(FinishReason, int64_t)> record_finish;
        std::function<void(const ServingContext &)> record_rag_metrics;
    };

    struct NonStreamResult
    {
        std::shared_ptr<ServingContext> ctx;
        bool client_closed = false;
    };

    ChatService(SessionExecutor &session_executor,
                EngineExecutor &engine_executor,
                ModelCatalogService &model_catalog_service,
                SessionManager &session_manager,
                ExtensionHooks extension_hooks,
                Callbacks callbacks);

    ChatError ValidateRequest(const ChatExecutionRequest &request) const;

    NonStreamResult RunNonStream(const ChatExecutionRequest &request,
                                 const std::function<void(std::function<void()>)> &bind_on_close,
                                 const std::function<bool()> &is_client_alive,
                                 std::chrono::steady_clock::time_point start_time) const;

    void RunStream(const ChatExecutionRequest &request,
                   const std::function<void(std::function<void()>)> &bind_on_close,
                   const std::function<void()> &start_stream,
                   const std::function<void()> &close_stream,
                   std::chrono::steady_clock::time_point start_time) const;

    ChatError BuildError(const std::shared_ptr<ServingContext> &ctx) const;
    ChatResponse BuildResponse(const std::shared_ptr<ServingContext> &ctx) const;

private:
    void LogChatStart(const std::shared_ptr<ServingContext> &ctx, bool stream) const;
    void AttachNonStreamFinishHandler(const ChatExecutionRequest &request,
                                      std::chrono::steady_clock::time_point start_time) const;
    void AttachStreamFinishHandler(const ChatExecutionRequest &request,
                                   const std::function<void()> &close_stream,
                                   std::chrono::steady_clock::time_point start_time) const;
    bool SubmitChatExecution(const ChatExecutionRequest &request) const;
    void UpdateSessionHistory(const ChatExecutionRequest &request) const;

private:
    SessionExecutor &session_executor_;
    EngineExecutor &engine_executor_;
    ModelCatalogService &model_catalog_service_;
    SessionManager &session_manager_;
    ExtensionHooks extension_hooks_;
    Callbacks callbacks_;
};
