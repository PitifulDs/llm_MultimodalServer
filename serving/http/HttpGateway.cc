#include "HttpGateway.h"

#include "ChatRequestParser.h"
#include "HttpGatewayInternal.h"
#include "HttpStreamSession.h"
#include "HttpUtils.h"
#include "OpenAIStreamWriter.h"
#include "http_types.h"
#include "serving/core/ServingContext.h"
#include "serving/core/SessionManager.h"
#include "serving/service/RequestLogging.h"

#include "../../utils/json.hpp"
#include <glog/logging.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace http_gateway_internal;

HttpGateway::HttpGateway()
    : pool_(GetWorkerThreads()),
      executor_(pool_),
      session_executor_(pool_),
      start_time_(std::chrono::steady_clock::now())
{
    SessionManager::Options opt;
    opt.idle_ttl = std::chrono::minutes(30);
    opt.max_sessions = 1024;
    opt.gc_batch = 64;

    session_mgr_ = std::make_unique<SessionManager>(opt);

    const auto repo_root = DetectRepoRoot();

    RAGExecutor::Options rag_opt;
    rag_opt.index_path = GetEnvString("RAG_INDEX_PATH", (repo_root / "data/rag_index.sqlite").string().c_str());
    rag_opt.vector_index_path = GetEnvString("RAG_VECTOR_INDEX_PATH", (repo_root / "data/rag_faiss.index").string().c_str());
    rag_opt.chunk_metadata_path = GetEnvString("RAG_CHUNK_METADATA_PATH", (repo_root / "data/rag_chunks.jsonl").string().c_str());
    rag_opt.vector_embeddings_path = GetEnvString("RAG_EMBEDDINGS_PATH", (repo_root / "data/rag_embeddings.npy").string().c_str());
    rag_opt.vector_id_map_path = GetEnvString("RAG_ID_MAP_PATH", (repo_root / "data/rag_id_map.json").string().c_str());
    rag_opt.default_top_k = GetEnvInt("RAG_DEFAULT_TOP_K", 6);
    rag_opt.max_context_chars = static_cast<size_t>(GetEnvInt("RAG_MAX_CONTEXT_CHARS", 6000));
    rag_opt.default_mode = GetEnvString("RAG_DEFAULT_MODE", "lexical");
    rag_opt.default_fusion = GetEnvString("RAG_DEFAULT_FUSION", "rrf");
    rag_opt.enable_neighbor_expand = GetEnvBool("RAG_ENABLE_NEIGHBOR_EXPAND", true);
    rag_opt.max_neighbor_count = GetEnvInt("RAG_MAX_NEIGHBOR_COUNT", 1);
    rag_executor_ = std::make_unique<RAGExecutor>(std::move(rag_opt));
    experimental_agent_api_enabled_ = GetEnvBool("EXPERIMENTAL_AGENT_API_ENABLED", false);
    experimental_rag_api_enabled_ = GetEnvBool("EXPERIMENTAL_RAG_API_ENABLED", false);
    rag_retrieval_debug_api_enabled_ = GetEnvBool("RAG_ENABLE_RETRIEVAL_DEBUG_API", true);
    max_concurrent_requests_ = GetEnvInt("MAX_CONCURRENT_REQUESTS", 0);
    max_model_concurrency_ = GetEnvInt("MAX_MODEL_CONCURRENCY", 0);
    max_session_concurrency_ = GetEnvInt("MAX_SESSION_CONCURRENCY", 0);
    request_timeout_ms_ = GetEnvInt("HTTP_REQUEST_TIMEOUT_MS", 0);

    AgentExecutor::Options agent_opt;
    agent_opt.repo_root = repo_root.string();
    agent_opt.docs_root = repo_root.string();
    agent_opt.config_path = (repo_root / "config.json").string();
    if (const char *cfg = std::getenv("CONFIG_PATH"))
    {
        if (*cfg)
            agent_opt.config_path = cfg;
    }
    if (rag_executor_)
    {
        agent_opt.search_kb_handler = [this](const json &input)
        {
            if (!rag_executor_)
                return std::string("rag executor unavailable.");

            RetrievalRequest request;
            request.kb = input.value("kb", "repo_code");
            request.query = input.value("query", "");
            request.top_k = input.value("top_k", 5);
            request.mode = input.value("mode", "hybrid");
            request.debug = true;

            RetrievalResponse response;
            std::string error;
            if (!rag_executor_->Search(request, response, error))
                return error.empty() ? std::string("search_kb failed.") : error;

            std::ostringstream oss;
            oss << "KB search hits for query: " << request.query << "\n";
            for (const auto &hit : response.hits)
            {
                oss << "- chunk_id=" << hit.chunk.chunk_id
                    << " path=" << hit.chunk.path
                    << ":" << hit.chunk.start_line << "-" << hit.chunk.end_line
                    << " symbol=" << hit.chunk.symbol
                    << " score=" << hit.final_score << "\n";
                std::string snippet = hit.chunk.text;
                std::replace(snippet.begin(), snippet.end(), '\n', ' ');
                if (snippet.size() > 180)
                    snippet = snippet.substr(0, 180) + "...";
                oss << "  snippet=" << snippet << "\n";
            }
            return oss.str();
        };
        agent_opt.open_chunk_handler = [this](const json &input)
        {
            if (!rag_executor_)
                return std::string("rag executor unavailable.");
            const std::string chunk_id = input.value("chunk_id", "");
            if (chunk_id.empty())
                return std::string("open_chunk requires chunk_id.");

            RagChunk chunk;
            std::string error;
            if (!rag_executor_->OpenChunk(chunk_id, chunk, error))
                return error.empty() ? std::string("chunk not found.") : error;

            std::ostringstream oss;
            oss << "CHUNK " << chunk.chunk_id << "\n"
                << "kb=" << chunk.kb_name << " path=" << chunk.path
                << " lines " << chunk.start_line << "-" << chunk.end_line
                << " symbol=" << chunk.symbol << "\n"
                << chunk.text << "\n";
            return oss.str();
        };
    }
    agent_executor_ = std::make_unique<AgentExecutor>(executor_, agent_opt);
    embeddings_service_ = std::make_unique<EmbeddingsService>(model_catalog_service_);
    rerank_service_ = std::make_unique<RerankService>(model_catalog_service_);
    chat_service_ = std::make_unique<ChatService>(
        session_executor_,
        executor_,
        model_catalog_service_,
        *session_mgr_,
        ChatService::ExtensionHooks{
            [this](const std::shared_ptr<ServingContext> &ctx)
            {
                if (!ctx || !ctx->rag_options.enabled)
                    return true;
                if (!rag_executor_)
                {
                    ctx->error_message = "rag executor unavailable";
                    ctx->params["error_code"] = "rag_unavailable";
                    return false;
                }
                return rag_executor_->Apply(ctx);
            },
            [this](const std::shared_ptr<ServingContext> &ctx)
            {
                if (!ctx || !ctx->use_agent)
                    return false;
                if (!agent_executor_)
                {
                    ctx->error_message = "agent executor unavailable";
                    ctx->params["error_code"] = "agent_unavailable";
                    ctx->EmitFinish(FinishReason::error);
                    return true;
                }
                agent_executor_->Run(ctx);
                return true;
            },
            [](const std::shared_ptr<ServingContext> &ctx, ChatError &error)
            {
                if (!ctx)
                    return false;

                const std::string error_code =
                    ctx->params.count("error_code") ? ctx->params.at("error_code") : std::string();

                if (error_code == "rag_invalid_request" || error_code == "rag_no_user_query")
                {
                    error = {
                        ChatErrorKind::InvalidRequest,
                        error_code,
                        ctx->error_message.empty() ? "invalid rag request" : ctx->error_message};
                    return true;
                }

                if (error_code == "rag_index_missing" || error_code == "rag_unavailable")
                {
                    error = {
                        ChatErrorKind::ServiceUnavailable,
                        error_code.empty() ? "rag_unavailable" : error_code,
                        ctx->error_message.empty() ? "rag unavailable" : ctx->error_message};
                    return true;
                }

                return false;
            },
            [](const std::shared_ptr<ServingContext> &ctx, ChatResponse &response)
            {
                if (!ctx)
                    return;

                if (ctx->rag_options.enabled && ctx->rag_options.return_references)
                    response.references = http_utils::build_rag_references(ctx->rag_hits);

                if (ctx->rag_options.enabled && ctx->rag_options.debug)
                {
                    response.retrieval = {
                        {"normalized_query", ctx->rag_summary.normalized_query},
                        {"mode", ctx->rag_summary.mode},
                        {"fusion", ctx->rag_summary.fusion},
                        {"lexical_hit_count", ctx->rag_summary.lexical_hit_count},
                        {"vector_hit_count", ctx->rag_summary.vector_hit_count},
                        {"final_hit_count", ctx->rag_summary.final_hit_count},
                        {"injected_chars", ctx->rag_summary.injected_chars},
                        {"retrieval_latency_ms", ctx->rag_summary.retrieval_latency_ms},
                    };
                }

                if (!ctx->use_agent)
                    return;

                const bool expose_agent_result = ctx->agent_debug || ctx->agent_output_format == "structured";
                const bool expose_agent_debug_payload = ctx->agent_debug;
                const bool expose_agent_trace = ctx->agent_debug || ctx->agent_include_trace;

                if (!ctx->agent_structured_output.empty() && expose_agent_result)
                    response.agent_result = ctx->agent_structured_output;

                if (!ctx->agent_structured_output.empty() && ctx->agent_structured_output.contains("references"))
                {
                    if (response.references.is_array() && ctx->agent_structured_output["references"].is_array())
                    {
                        for (const auto &item : ctx->agent_structured_output["references"])
                            response.references.push_back(item);
                    }
                    else
                    {
                        response.references = ctx->agent_structured_output["references"];
                    }
                }

                if (!ctx->agent_structured_output.empty() &&
                    ctx->agent_structured_output.contains("subqueries") &&
                    expose_agent_debug_payload)
                    response.subqueries = ctx->agent_structured_output["subqueries"];

                if (!ctx->agent_evidence.empty() && expose_agent_debug_payload)
                {
                    response.evidence = nlohmann::json::array();
                    for (const auto &item : ctx->agent_evidence)
                        response.evidence.push_back(ToJson(item));
                }

                if (expose_agent_trace)
                {
                    response.agent_trace = nlohmann::json::array();
                    for (const auto &item : ctx->agent_trace)
                        response.agent_trace.push_back(ToJson(item));
                }
            }},
        ChatService::Callbacks{
            [this](FinishReason reason, int64_t dur_ms)
            {
                RecordFinish(reason, dur_ms);
            },
            [this](const ServingContext &ctx)
            {
                RecordRagMetrics(ctx);
            },
            [this](const ServingContext &ctx, FinishReason reason, int64_t run_ms)
            {
                const auto queue_wait_ms = ParseInt64OrDefault(
                    ctx.params.count("queue_wait_ms") ? ctx.params.at("queue_wait_ms") : std::string(),
                    0);
                const std::string error_code =
                    ctx.params.count("error_code") ? ctx.params.at("error_code") : std::string();
                const int status_code = reason == FinishReason::cancelled
                                            ? 499
                                            : (reason == FinishReason::error
                                                   ? BuildChatPlatformError(ctx).HttpStatus()
                                                   : 200);

                RecordGovernedRequest(ctx.request_id,
                                      "/v1/chat/completions",
                                      ctx.model,
                                      ctx.params.count("resolved_backend") ? ctx.params.at("resolved_backend")
                                                                           : ctx.inference_backend,
                                      ctx.capability,
                                      ctx.session_id,
                                      reason,
                                      status_code,
                                      error_code,
                                      queue_wait_ms,
                                      run_ms,
                                      ctx.usage.prompt_tokens,
                                      ctx.usage.completion_tokens,
                                      ctx.usage.total_tokens,
                                      ctx.stream);
            }});
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
            {"requests_cancelled_total", cancelled_requests_.load(std::memory_order_relaxed)},
            {"requests_timeout_total", timeout_requests_.load(std::memory_order_relaxed)},
            {"requests_rate_limited_total", rate_limited_requests_.load(std::memory_order_relaxed)},
            {"total_tokens_total", total_tokens_total_.load(std::memory_order_relaxed)}};
        return out.dump();
    });

    // Session GC 后台线程（可停止，避免悬空指针）
    gc_thread_ = std::thread([this]()
    {
        std::unique_lock<std::mutex> lk(gc_mu_);
        while (!stop_gc_)
        {
            if (gc_cv_.wait_for(lk, std::chrono::seconds(60), [this]
                                { return stop_gc_; }))
            {
                break;
            }

            lk.unlock();
            const size_t removed = session_mgr_ ? session_mgr_->gc() : 0;
            if (removed > 0 && session_mgr_)
            {
                LOG(INFO) << "[session-gc] removed=" << removed
                          << " remaining=" << session_mgr_->size();
            }
            lk.lock();
        }
    });
}

HttpGateway::~HttpGateway()
{
    {
        std::lock_guard<std::mutex> lk(gc_mu_);
        stop_gc_ = true;
    }
    gc_cv_.notify_all();
    if (gc_thread_.joinable())
        gc_thread_.join();
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

void HttpGateway::WriteError(HttpResponse &res, const PlatformError &error, const std::string &param)
{
    WriteError(res, error.HttpStatus(), error.message, error.HttpType(), error.code, param);
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

void HttpGateway::RecordGovernedRequest(const std::string &request_id,
                                        const std::string &api,
                                        const std::string &model,
                                        const std::string &backend,
                                        ModelCapability capability,
                                        const std::string &session_id,
                                        FinishReason reason,
                                        int status_code,
                                        const std::string &error_code,
                                        int64_t queue_wait_ms,
                                        int64_t run_ms,
                                        int prompt_tokens,
                                        int completion_tokens,
                                        int total_tokens,
                                        bool stream)
{
    LogPlatformRequest("finish", RequestLogRecord{
                                     request_id,
                                     api,
                                     model,
                                     backend,
                                     std::string(ToString(capability)),
                                     session_id,
                                     queue_wait_ms,
                                     run_ms,
                                     FinishReasonToStr(reason),
                                     status_code,
                                     error_code,
                                     prompt_tokens,
                                     completion_tokens,
                                     total_tokens,
                                     stream});

    prompt_tokens_total_.fetch_add(prompt_tokens, std::memory_order_relaxed);
    completion_tokens_total_.fetch_add(completion_tokens, std::memory_order_relaxed);
    total_tokens_total_.fetch_add(total_tokens, std::memory_order_relaxed);
    if (IsPlatformTimeoutCode(error_code))
        timeout_requests_.fetch_add(1, std::memory_order_relaxed);
    if (IsPlatformRateLimitCode(error_code))
        rate_limited_requests_.fetch_add(1, std::memory_order_relaxed);

    if (backend.empty())
        return;

    std::lock_guard<std::mutex> lk(backend_governance_mu_);
    auto &stats = backend_governance_[backend];
    stats.requests_total += 1;
    stats.prompt_tokens_total += prompt_tokens;
    stats.completion_tokens_total += completion_tokens;
    stats.total_tokens_total += total_tokens;
    if (reason == FinishReason::error)
    {
        stats.requests_error_total += 1;
        stats.last_error = error_code.empty() ? "internal_error" : error_code;
        if (IsPlatformTimeoutCode(error_code))
        {
            stats.requests_timeout_total += 1;
            stats.timeout_total += 1;
        }
        if (IsPlatformRateLimitCode(error_code))
            stats.requests_rate_limited_total += 1;
    }
    else if (reason == FinishReason::cancelled)
    {
        stats.requests_cancelled_total += 1;
        stats.cancelled_total += 1;
    }
}

void HttpGateway::RecordRagMetrics(const ServingContext &ctx)
{
    rag_requests_total_.fetch_add(1, std::memory_order_relaxed);
    if (ctx.rag_options.kb == "docs")
        rag_requests_docs_total_.fetch_add(1, std::memory_order_relaxed);
    else if (ctx.rag_options.kb == "repo_code")
        rag_requests_repo_code_total_.fetch_add(1, std::memory_order_relaxed);

    if (ctx.rag_summary.mode == "lexical")
        rag_mode_lexical_total_.fetch_add(1, std::memory_order_relaxed);
    else if (ctx.rag_summary.mode == "vector")
        rag_mode_vector_total_.fetch_add(1, std::memory_order_relaxed);
    else if (ctx.rag_summary.mode == "hybrid")
        rag_mode_hybrid_total_.fetch_add(1, std::memory_order_relaxed);

    rag_retrieval_latency_ms_total_.fetch_add(ctx.rag_summary.retrieval_latency_ms, std::memory_order_relaxed);
    rag_hit_count_total_.fetch_add(static_cast<int64_t>(ctx.rag_hits.size()), std::memory_order_relaxed);
    rag_injected_chars_total_.fetch_add(ctx.rag_summary.injected_chars, std::memory_order_relaxed);
    rag_vector_search_latency_ms_total_.fetch_add(ctx.rag_summary.vector_search_latency_ms, std::memory_order_relaxed);
    rag_lexical_search_latency_ms_total_.fetch_add(ctx.rag_summary.lexical_search_latency_ms, std::memory_order_relaxed);
    if (ctx.rag_hits.empty())
        rag_empty_hit_total_.fetch_add(1, std::memory_order_relaxed);
}

PlatformRuntimeSnapshot HttpGateway::BuildPlatformRuntimeSnapshot() const
{
    PlatformRuntimeSnapshot snapshot;
    snapshot.uptime_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_)
            .count();
    snapshot.requests_total = total_requests_.load(std::memory_order_relaxed);
    snapshot.requests_in_flight = in_flight_.load(std::memory_order_relaxed);
    snapshot.requests_stream_total = stream_requests_.load(std::memory_order_relaxed);
    snapshot.requests_error_total = error_requests_.load(std::memory_order_relaxed);
    snapshot.requests_cancelled_total = cancelled_requests_.load(std::memory_order_relaxed);
    snapshot.requests_timeout_total = timeout_requests_.load(std::memory_order_relaxed);
    snapshot.requests_rate_limited_total = rate_limited_requests_.load(std::memory_order_relaxed);
    snapshot.prompt_tokens_total = prompt_tokens_total_.load(std::memory_order_relaxed);
    snapshot.completion_tokens_total = completion_tokens_total_.load(std::memory_order_relaxed);
    snapshot.total_tokens_total = total_tokens_total_.load(std::memory_order_relaxed);
    return snapshot;
}

std::vector<BackendRuntimeSnapshot> HttpGateway::BuildBackendRuntimeSnapshots() const
{
    auto snapshots = executor_.GetBackendRuntimeSnapshots();
    std::unordered_map<std::string, BackendRuntimeSnapshot> merged;
    for (const auto &snapshot : snapshots)
        merged[snapshot.backend] = snapshot;

    std::lock_guard<std::mutex> lk(backend_governance_mu_);
    for (const auto &[backend, counters] : backend_governance_)
    {
        auto &snapshot = merged[backend];
        snapshot.backend = backend;
        snapshot.requests_total += counters.requests_total;
        snapshot.requests_error_total += counters.requests_error_total;
        snapshot.requests_cancelled_total += counters.requests_cancelled_total;
        snapshot.requests_timeout_total += counters.requests_timeout_total;
        snapshot.requests_rate_limited_total += counters.requests_rate_limited_total;
        snapshot.timeout_total += counters.timeout_total;
        snapshot.cancelled_total += counters.cancelled_total;
        snapshot.prompt_tokens_total += counters.prompt_tokens_total;
        snapshot.completion_tokens_total += counters.completion_tokens_total;
        snapshot.total_tokens_total += counters.total_tokens_total;
        if (!counters.last_error.empty())
            snapshot.last_error = counters.last_error;
    }

    std::vector<BackendRuntimeSnapshot> out;
    out.reserve(merged.size());
    for (auto &[_, snapshot] : merged)
        out.push_back(snapshot);
    return out;
}

void HttpGateway::WriteExperimentalApiDisabled(HttpResponse &res,
                                               const std::string &route,
                                               const std::string &env_name)
{
    WriteError(res,
               404,
               BuildExperimentalApiDisabledMessage(route, env_name),
               "invalid_request_error",
               "experimental_api_disabled");
}

void HttpGateway::HandleChatCompletion(const HttpRequest &req, HttpResponse &res)
{
    const auto start_time = std::chrono::steady_clock::now();
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    in_flight_.fetch_add(1, std::memory_order_relaxed);

    const std::string request_id = GenRequestId();
    auto parsed = ParseChatRequestBody(req.body, false, *session_mgr_, GetDefaultModel(), GetDefaultMaxTokens(), request_id);
    if (!parsed.ok)
    {
        WriteError(res, parsed.status, parsed.message, parsed.type, parsed.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(request_id,
                              "/v1/chat/completions",
                              "",
                              "",
                              ModelCapability::Chat,
                              "",
                              FinishReason::error,
                              parsed.status,
                              parsed.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }
    auto ctx = parsed.request.ctx;

    if (!chat_service_)
    {
        const PlatformError error{
            PlatformErrorKind::Internal,
            "chat_service_unavailable",
            "chat service unavailable"};
        WriteError(res, error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(ctx ? ctx->request_id : request_id,
                              "/v1/chat/completions",
                              ctx ? ctx->model : std::string(),
                              ctx ? ctx->inference_backend : std::string(),
                              ModelCapability::Chat,
                              ctx ? ctx->session_id : std::string(),
                              FinishReason::error,
                              error.HttpStatus(),
                              error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const ChatExecutionRequest chat_request{parsed.request.ctx, parsed.request.client_messages};
    const ChatError validation_error = chat_service_->ValidateRequest(chat_request);
    if (validation_error.HasError())
    {
        WriteError(res, validation_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(ctx ? ctx->request_id : request_id,
                              "/v1/chat/completions",
                              ctx ? ctx->model : std::string(),
                              ctx ? ctx->inference_backend : std::string(),
                              ModelCapability::Chat,
                              ctx ? ctx->session_id : std::string(),
                              FinishReason::error,
                              validation_error.HttpStatus(),
                              validation_error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    std::shared_ptr<void> request_lease;
    const PlatformError limit_error = AcquireRequestLease(ctx ? ctx->model : std::string(),
                                                          ctx ? ctx->session_id : std::string(),
                                                          request_lease);
    if (limit_error.HasError())
    {
        WriteError(res, limit_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(ctx ? ctx->request_id : request_id,
                              "/v1/chat/completions",
                              ctx ? ctx->model : std::string(),
                              ctx ? ctx->inference_backend : std::string(),
                              ModelCapability::Chat,
                              ctx ? ctx->session_id : std::string(),
                              FinishReason::error,
                              limit_error.HttpStatus(),
                              limit_error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }
    ctx->request_state = request_lease;
    if (request_timeout_ms_ > 0)
        ctx->deadline = start_time + std::chrono::milliseconds(request_timeout_ms_);
    if (HasDeadline(ctx->deadline))
    {
        std::weak_ptr<ServingContext> weak_ctx = ctx;
        std::thread([weak_ctx]
        {
            while (auto live = weak_ctx.lock())
            {
                if (live->finished.load(std::memory_order_acquire))
                    return;
                if (live->DeadlineExceeded())
                {
                    live->cancelled.store(true, std::memory_order_release);
                    live->params["error_code"] = "request_timeout";
                    live->error_message = "request timed out";
                    live->EmitFinish(FinishReason::error);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
    }

    auto run_result = chat_service_->RunNonStream(
        chat_request,
        [&res](std::function<void()> on_close)
        {
            res.SetOnClose(std::move(on_close));
        },
        [&res]()
        {
            return res.IsAlive();
        },
        start_time);

    if (run_result.client_closed || !run_result.ctx)
        return;

    ctx = run_result.ctx;
    const ChatError error = chat_service_->BuildError(ctx);
    if (error.HasError())
    {
        WriteError(res, error);
        return;
    }

    const ChatResponse response = chat_service_->BuildResponse(ctx);
    json out = BuildChatCompletionJson(ctx->request_id, response);

    res.SetStatus(200, "OK");
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

    const std::string request_id = GenRequestId();
    auto parsed = ParseChatRequestBody(req.body, true, *session_mgr_, GetDefaultModel(), GetDefaultMaxTokens(), request_id);
    if (!parsed.ok)
    {
        WriteError(*res_ptr, parsed.status, parsed.message, parsed.type, parsed.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(request_id,
                              "/v1/chat/completions",
                              "",
                              "",
                              ModelCapability::Chat,
                              "",
                              FinishReason::error,
                              parsed.status,
                              parsed.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              true);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }
    auto ctx = parsed.request.ctx;

    if (!chat_service_)
    {
        const PlatformError error{
            PlatformErrorKind::Internal,
            "chat_service_unavailable",
            "chat service unavailable"};
        WriteError(*res_ptr, error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(ctx ? ctx->request_id : request_id,
                              "/v1/chat/completions",
                              ctx ? ctx->model : std::string(),
                              ctx ? ctx->inference_backend : std::string(),
                              ModelCapability::Chat,
                              ctx ? ctx->session_id : std::string(),
                              FinishReason::error,
                              error.HttpStatus(),
                              error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              true);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const ChatExecutionRequest chat_request{parsed.request.ctx, parsed.request.client_messages};
    const ChatError validation_error = chat_service_->ValidateRequest(chat_request);
    if (validation_error.HasError())
    {
        WriteError(*res_ptr, validation_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(ctx ? ctx->request_id : request_id,
                              "/v1/chat/completions",
                              ctx ? ctx->model : std::string(),
                              ctx ? ctx->inference_backend : std::string(),
                              ModelCapability::Chat,
                              ctx ? ctx->session_id : std::string(),
                              FinishReason::error,
                              validation_error.HttpStatus(),
                              validation_error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              true);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    std::shared_ptr<void> request_lease;
    const PlatformError limit_error = AcquireRequestLease(ctx ? ctx->model : std::string(),
                                                          ctx ? ctx->session_id : std::string(),
                                                          request_lease);
    if (limit_error.HasError())
    {
        WriteError(*res_ptr, limit_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(ctx ? ctx->request_id : request_id,
                              "/v1/chat/completions",
                              ctx ? ctx->model : std::string(),
                              ctx ? ctx->inference_backend : std::string(),
                              ModelCapability::Chat,
                              ctx ? ctx->session_id : std::string(),
                              FinishReason::error,
                              limit_error.HttpStatus(),
                              limit_error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              true);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }
    ctx->request_state = request_lease;
    if (request_timeout_ms_ > 0)
        ctx->deadline = start_time + std::chrono::milliseconds(request_timeout_ms_);
    if (HasDeadline(ctx->deadline))
    {
        std::weak_ptr<ServingContext> weak_ctx = ctx;
        std::thread([weak_ctx]
        {
            while (auto live = weak_ctx.lock())
            {
                if (live->finished.load(std::memory_order_acquire))
                    return;
                if (live->DeadlineExceeded())
                {
                    live->cancelled.store(true, std::memory_order_release);
                    live->params["error_code"] = "request_timeout";
                    live->error_message = "request timed out";
                    live->EmitFinish(FinishReason::error);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
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
        writer->OnChunk(chunk);
    };

    chat_service_->RunStream(
        chat_request,
        [res_ptr](std::function<void()> on_close)
        {
            res_ptr->SetOnClose(std::move(on_close));
        },
        [http_session]()
        {
            http_session->Start();
        },
        [http_session]()
        {
            http_session->Close();
        },
        start_time);
}

PlatformError HttpGateway::AcquireRequestLease(const std::string &model,
                                               const std::string &session_id,
                                               std::shared_ptr<void> &lease)
{
    struct Lease
    {
        HttpGateway *owner = nullptr;
        std::string model;
        std::string session_id;

        ~Lease()
        {
            if (owner)
                owner->ReleaseRequestLease(model, session_id);
        }
    };

    std::lock_guard<std::mutex> lk(request_limit_mu_);
    if (max_concurrent_requests_ > 0 && governed_in_flight_ >= max_concurrent_requests_)
    {
        return {
            PlatformErrorKind::RateLimit,
            "rate_limit_global",
            "global request concurrency limit reached"};
    }

    if (max_model_concurrency_ > 0 && !model.empty())
    {
        const auto it = model_in_flight_.find(model);
        const int64_t current = it == model_in_flight_.end() ? 0 : it->second;
        if (current >= max_model_concurrency_)
        {
            return {
                PlatformErrorKind::RateLimit,
                "rate_limit_model",
                "model concurrency limit reached: " + model};
        }
    }

    if (max_session_concurrency_ > 0 && !session_id.empty())
    {
        const auto it = session_in_flight_.find(session_id);
        const int64_t current = it == session_in_flight_.end() ? 0 : it->second;
        if (current >= max_session_concurrency_)
        {
            return {
                PlatformErrorKind::RateLimit,
                "rate_limit_session",
                "session concurrency limit reached: " + session_id};
        }
    }

    governed_in_flight_ += 1;
    if (!model.empty())
        model_in_flight_[model] += 1;
    if (!session_id.empty())
        session_in_flight_[session_id] += 1;

    auto owned_lease = std::make_shared<Lease>();
    owned_lease->owner = this;
    owned_lease->model = model;
    owned_lease->session_id = session_id;
    lease = owned_lease;
    return {};
}

void HttpGateway::ReleaseRequestLease(const std::string &model, const std::string &session_id)
{
    std::lock_guard<std::mutex> lk(request_limit_mu_);
    if (governed_in_flight_ > 0)
        governed_in_flight_ -= 1;

    if (!model.empty())
    {
        auto it = model_in_flight_.find(model);
        if (it != model_in_flight_.end())
        {
            if (it->second > 0)
                it->second -= 1;
            if (it->second <= 0)
                model_in_flight_.erase(it);
        }
    }

    if (!session_id.empty())
    {
        auto it = session_in_flight_.find(session_id);
        if (it != session_in_flight_.end())
        {
            if (it->second > 0)
                it->second -= 1;
            if (it->second <= 0)
                session_in_flight_.erase(it);
        }
    }
}
