#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "protocol/Protocol.h"
#include "engine/ModelRegistry.h"
#include "serving/core/agent/AgentExecutor.h"
#include "serving/core/SessionManager.h"
#include "serving/core/EngineExecutor.h"
#include "serving/core/SessionExecutor.h"
#include "serving/core/ThreadPool.h"
#include "serving/service/AdminStatusService.h"
#include "serving/service/ChatService.h"
#include "serving/service/EmbeddingsService.h"
#include "serving/service/HealthService.h"
#include "serving/service/ModelCatalogService.h"
#include "serving/service/StatusTypes.h"
#include "serving/rag/RAGExecutor.h"

// 前向声明
struct HttpRequest;
struct HttpResponse;
class StackFlowsClient;
struct ServingContext;
enum class FinishReason;

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
    ~HttpGateway();

    // 非流式 completion
    void HandleCompletion(const HttpRequest &req, HttpResponse &res);

    // 流式 completion（SSE）
    void HandleCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr);

    // 新增：Chat
    void HandleChatCompletion(const HttpRequest &req, HttpResponse &res);
    void HandleChatCompletionStream(const HttpRequest &req, std::shared_ptr<HttpResponse> res_ptr);
    void HandleEmbeddings(const HttpRequest &req, HttpResponse &res);

    // 健康检查 / 指标
    void HandleHealth(const HttpRequest &req, HttpResponse &res);
    void HandleHealthz(const HttpRequest &req, HttpResponse &res);
    void HandleMetrics(const HttpRequest &req, HttpResponse &res);
    void HandleModels(const HttpRequest &req, HttpResponse &res);
    void HandleAdminModelsStatus(const HttpRequest &req, HttpResponse &res);
    void HandleAdminBackendsStatus(const HttpRequest &req, HttpResponse &res);
    void HandleRetrievalSearch(const HttpRequest &req, HttpResponse &res);
    void HandleAgentDebug(const HttpRequest &req, HttpResponse &res);
    void HandleAdminRagReloadIndex(const HttpRequest &req, HttpResponse &res);
    void HandleAdminRagStatus(const HttpRequest &req, HttpResponse &res);

private:
    void WriteError(HttpResponse &res, int status, const std::string &message,
                    const std::string &type, const std::string &code = "",
                    const std::string &param = "");
    void RecordFinish(FinishReason reason, int64_t dur_ms);
    void RecordRagMetrics(const ServingContext &ctx);
    PlatformRuntimeSnapshot BuildPlatformRuntimeSnapshot() const;

    ThreadPool pool_;                        // 线程池
    StackFlowsClient *sf_client_{nullptr};   // 不持有所有权
    std::unique_ptr<SessionManager> session_mgr_;
    EngineExecutor executor_; // 共享一个 executor，所有请求都走这里
    std::unique_ptr<AgentExecutor> agent_executor_;
    std::unique_ptr<RAGExecutor> rag_executor_;
    std::unique_ptr<ChatService> chat_service_;
    std::unique_ptr<EmbeddingsService> embeddings_service_;
    HealthService health_service_;
    AdminStatusService admin_status_service_;
    ModelCatalogService model_catalog_service_;
    SessionExecutor session_executor_;

    std::chrono::steady_clock::time_point start_time_;
    std::atomic<int64_t> total_requests_{0};
    std::atomic<int64_t> stream_requests_{0};
    std::atomic<int64_t> error_requests_{0};
    std::atomic<int64_t> cancelled_requests_{0};
    std::atomic<int64_t> in_flight_{0};
    std::atomic<int64_t> total_latency_ms_{0};
    std::atomic<int64_t> rag_requests_total_{0};
    std::atomic<int64_t> rag_requests_docs_total_{0};
    std::atomic<int64_t> rag_requests_repo_code_total_{0};
    std::atomic<int64_t> rag_mode_lexical_total_{0};
    std::atomic<int64_t> rag_mode_vector_total_{0};
    std::atomic<int64_t> rag_mode_hybrid_total_{0};
    std::atomic<int64_t> rag_retrieval_latency_ms_total_{0};
    std::atomic<int64_t> rag_hit_count_total_{0};
    std::atomic<int64_t> rag_empty_hit_total_{0};
    std::atomic<int64_t> rag_injected_chars_total_{0};
    std::atomic<int64_t> rag_vector_search_latency_ms_total_{0};
    std::atomic<int64_t> rag_lexical_search_latency_ms_total_{0};
    bool rag_retrieval_debug_api_enabled_{true};

    std::mutex gc_mu_;
    std::condition_variable gc_cv_;
    bool stop_gc_{false};
    std::thread gc_thread_;
};
