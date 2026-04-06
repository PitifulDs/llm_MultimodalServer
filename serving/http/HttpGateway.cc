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
        return ModelRegistry::GetDefaultModel();
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

    std::string get_env_string(const char *name, const char *def_val = "")
    {
        const char *env = std::getenv(name);
        if (!env || !*env)
            return std::string(def_val);
        return std::string(env);
    }

    int get_env_int(const char *name, int def)
    {
        const char *env = std::getenv(name);
        if (!env || !*env)
            return def;
        try
        {
            const int v = std::stoi(env);
            return v > 0 ? v : def;
        }
        catch (...)
        {
            return def;
        }
    }

    bool get_env_bool(const char *name, bool def)
    {
        const char *env = std::getenv(name);
        if (!env || !*env)
            return def;
        std::string value(env);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                       { return static_cast<char>(std::tolower(ch)); });
        if (value == "1" || value == "true" || value == "yes")
            return true;
        if (value == "0" || value == "false" || value == "no")
            return false;
        return def;
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

    int status_from_chat_error_kind(ChatErrorKind kind)
    {
        switch (kind)
        {
        case ChatErrorKind::InvalidRequest:
            return 400;
        case ChatErrorKind::ServiceUnavailable:
            return 503;
        case ChatErrorKind::RateLimit:
            return 429;
        case ChatErrorKind::Internal:
            return 500;
        case ChatErrorKind::None:
        default:
            return 500;
        }
    }

    const char *type_from_chat_error_kind(ChatErrorKind kind)
    {
        switch (kind)
        {
        case ChatErrorKind::InvalidRequest:
            return "invalid_request_error";
        case ChatErrorKind::ServiceUnavailable:
            return "service_unavailable_error";
        case ChatErrorKind::RateLimit:
            return "rate_limit_error";
        case ChatErrorKind::Internal:
        case ChatErrorKind::None:
        default:
            return "internal_error";
        }
    }

    json build_chat_completion_json(const std::string &request_id,
                                    const ChatResponse &response)
    {
        json out = {
            {"id", "chatcmpl-" + request_id},
            {"object", "chat.completion"},
            {"created", static_cast<int>(std::time(nullptr))},
            {"model", response.model},
            {"choices",
             {{{"index", 0},
               {"message", {{"role", "assistant"}, {"content", response.output_text}}},
               {"logprobs", nullptr},
               {"finish_reason", finish_reason_to_str(response.finish_reason)}}}},
            {"usage",
             {{"prompt_tokens", response.usage.prompt_tokens},
              {"completion_tokens", response.usage.completion_tokens},
              {"total_tokens", response.usage.total_tokens}}}
        };

        if (!response.references.is_null())
            out["references"] = response.references;
        if (!response.retrieval.is_null())
            out["retrieval"] = response.retrieval;
        if (!response.agent_result.is_null())
            out["agent_result"] = response.agent_result;
        if (!response.subqueries.is_null())
            out["subqueries"] = response.subqueries;
        if (!response.evidence.is_null())
            out["evidence"] = response.evidence;
        if (!response.agent_trace.is_null())
            out["agent_trace"] = response.agent_trace;

        return out;
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

    const auto repo_root = detect_repo_root();

    RAGExecutor::Options rag_opt;
    rag_opt.index_path = get_env_string("RAG_INDEX_PATH", (repo_root / "data/rag_index.sqlite").string().c_str());
    rag_opt.vector_index_path = get_env_string("RAG_VECTOR_INDEX_PATH", (repo_root / "data/rag_faiss.index").string().c_str());
    rag_opt.chunk_metadata_path = get_env_string("RAG_CHUNK_METADATA_PATH", (repo_root / "data/rag_chunks.jsonl").string().c_str());
    rag_opt.vector_embeddings_path = get_env_string("RAG_EMBEDDINGS_PATH", (repo_root / "data/rag_embeddings.npy").string().c_str());
    rag_opt.vector_id_map_path = get_env_string("RAG_ID_MAP_PATH", (repo_root / "data/rag_id_map.json").string().c_str());
    rag_opt.default_top_k = get_env_int("RAG_DEFAULT_TOP_K", 6);
    rag_opt.max_context_chars = static_cast<size_t>(get_env_int("RAG_MAX_CONTEXT_CHARS", 6000));
    rag_opt.default_mode = get_env_string("RAG_DEFAULT_MODE", "lexical");
    rag_opt.default_fusion = get_env_string("RAG_DEFAULT_FUSION", "rrf");
    rag_opt.enable_neighbor_expand = get_env_bool("RAG_ENABLE_NEIGHBOR_EXPAND", true);
    rag_opt.max_neighbor_count = get_env_int("RAG_MAX_NEIGHBOR_COUNT", 1);
    rag_executor_ = std::make_unique<RAGExecutor>(std::move(rag_opt));
    rag_retrieval_debug_api_enabled_ = get_env_bool("RAG_ENABLE_RETRIEVAL_DEBUG_API", true);

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

                if (!ctx->agent_structured_output.empty())
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

                if (!ctx->agent_structured_output.empty() && ctx->agent_structured_output.contains("subqueries"))
                    response.subqueries = ctx->agent_structured_output["subqueries"];

                if (!ctx->agent_evidence.empty())
                {
                    response.evidence = nlohmann::json::array();
                    for (const auto &item : ctx->agent_evidence)
                        response.evidence.push_back(ToJson(item));
                }

                if (ctx->agent_debug || ctx->agent_include_trace)
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
            {"requests_cancelled_total", cancelled_requests_.load(std::memory_order_relaxed)}};
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

void HttpGateway::RecordFinish(FinishReason reason, int64_t dur_ms)
{
    total_latency_ms_.fetch_add(dur_ms, std::memory_order_relaxed);
    in_flight_.fetch_sub(1, std::memory_order_relaxed);

    if (reason == FinishReason::error)
        error_requests_.fetch_add(1, std::memory_order_relaxed);
    else if (reason == FinishReason::cancelled)
        cancelled_requests_.fetch_add(1, std::memory_order_relaxed);
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
    return snapshot;
}

void HttpGateway::HandleHealth(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = health_service_.BuildHealth(BuildPlatformRuntimeSnapshot());

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}

void HttpGateway::HandleHealthz(const HttpRequest &req, HttpResponse &res)
{
    HandleHealth(req, res);
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
        {"avg_latency_ms", avg_latency_ms},
        {"rag_requests_total", rag_requests_total_.load(std::memory_order_relaxed)},
        {"rag_requests_total_by_kb",
         {
             {"docs", rag_requests_docs_total_.load(std::memory_order_relaxed)},
             {"repo_code", rag_requests_repo_code_total_.load(std::memory_order_relaxed)},
         }},
        {"rag_mode_total",
         {
             {"lexical", rag_mode_lexical_total_.load(std::memory_order_relaxed)},
             {"vector", rag_mode_vector_total_.load(std::memory_order_relaxed)},
             {"hybrid", rag_mode_hybrid_total_.load(std::memory_order_relaxed)},
         }},
        {"rag_retrieval_latency_ms", rag_retrieval_latency_ms_total_.load(std::memory_order_relaxed)},
        {"rag_hit_count", rag_hit_count_total_.load(std::memory_order_relaxed)},
        {"rag_empty_hit_total", rag_empty_hit_total_.load(std::memory_order_relaxed)},
        {"rag_injected_chars", rag_injected_chars_total_.load(std::memory_order_relaxed)},
        {"rag_vector_search_latency_ms", rag_vector_search_latency_ms_total_.load(std::memory_order_relaxed)},
        {"rag_lexical_search_latency_ms", rag_lexical_search_latency_ms_total_.load(std::memory_order_relaxed)}};

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
    const auto models = model_catalog_service_.ListModels();

    for (const auto &model : models)
    {
        json configured_backends = json::array();
        if (model.has_local)
            configured_backends.push_back("local");
        if (model.has_rpc)
            configured_backends.push_back("rpc");

        items.push_back({
            {"id", model.id},
            {"object", "model"},
            {"owned_by", "edge-llm-serving"},
            {"default", model.is_default},
            {"default_backend", model.default_backend},
            {"capabilities", model.capabilities},
            {"declared_backends", model.backends},
            // 真实配置能力（模型级）
            {"backends", configured_backends},
            // 网关支持的请求级后端切换能力（路由级）
            {"gateway_backends", json::array({"local", "rpc"})}
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

void HttpGateway::HandleAdminModelsStatus(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = admin_status_service_.BuildModelsStatus(model_catalog_service_.ListModels());

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}

void HttpGateway::HandleAdminBackendsStatus(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = admin_status_service_.BuildBackendsStatus(
        model_catalog_service_.ListModels(),
        executor_.GetBackendRuntimeSnapshots(),
        BuildPlatformRuntimeSnapshot());

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}

void HttpGateway::HandleRetrievalSearch(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    if (!rag_retrieval_debug_api_enabled_)
    {
        WriteError(res, 404, "retrieval debug api disabled", "invalid_request_error", "retrieval_debug_api_disabled");
        return;
    }
    if (!rag_executor_)
    {
        WriteError(res, 503, "rag executor unavailable", "service_unavailable_error", "rag_unavailable");
        return;
    }

    json body;
    try
    {
        body = json::parse(req.body.empty() ? "{}" : req.body);
    }
    catch (...)
    {
        WriteError(res, 400, "invalid json", "invalid_request_error", "invalid_json");
        return;
    }

    RetrievalRequest request;
    request.kb = body.value("kb", "repo_code");
    request.query = body.value("query", "");
    request.mode = body.value("mode", "lexical");
    request.top_k = body.value("top_k", 6);
    request.lexical_top_k = body.value("lexical_top_k", 0);
    request.vector_top_k = body.value("vector_top_k", 0);
    request.fusion = body.value("fusion", "rrf");
    request.debug = body.value("debug", true);
    if (request.query.empty())
    {
        WriteError(res, 400, "query is required", "invalid_request_error", "invalid_query");
        return;
    }

    RetrievalResponse response;
    std::string error;
    if (!rag_executor_->Search(request, response, error))
    {
        const int status = (error.find("not found") != std::string::npos || error.find("not loaded") != std::string::npos) ? 503 : 400;
        WriteError(res,
                   status,
                   error.empty() ? "retrieval search failed" : error,
                   status == 503 ? "service_unavailable_error" : "invalid_request_error",
                   "retrieval_search_failed");
        return;
    }

    json hits = json::array();
    for (const auto &hit : response.hits)
    {
        hits.push_back({
            {"chunk_id", hit.chunk.chunk_id},
            {"path", hit.chunk.path},
            {"start_line", hit.chunk.start_line},
            {"end_line", hit.chunk.end_line},
            {"symbol", hit.chunk.symbol},
            {"text", hit.chunk.text},
            {"lexical_score", hit.lexical_score},
            {"vector_score", hit.vector_score},
            {"final_score", hit.final_score},
            {"from_neighbor", hit.from_neighbor},
        });
    }

    json out = {
        {"normalized_query", response.normalized_query},
        {"mode", response.summary.mode},
        {"fusion", response.summary.fusion},
        {"hits", hits},
    };
    if (request.debug)
    {
        out["summary"] = {
            {"lexical_hit_count", response.summary.lexical_hit_count},
            {"vector_hit_count", response.summary.vector_hit_count},
            {"final_hit_count", response.summary.final_hit_count},
            {"retrieval_latency_ms", response.summary.retrieval_latency_ms},
            {"lexical_search_latency_ms", response.summary.lexical_search_latency_ms},
            {"vector_search_latency_ms", response.summary.vector_search_latency_ms},
        };
    }

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump(-1, ' ', false, json::error_handler_t::replace));
    res.End();
}

void HttpGateway::HandleAgentDebug(const HttpRequest &req, HttpResponse &res)
{
    const auto start_time = std::chrono::steady_clock::now();
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    in_flight_.fetch_add(1, std::memory_order_relaxed);

    json body;
    try
    {
        body = json::parse(req.body.empty() ? "{}" : req.body);
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

    const std::string query = body.value("query", "");
    if (query.empty())
    {
        WriteError(res, 400, "query is required", "invalid_request_error", "invalid_query");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    std::string mode = body.value("mode", "code_analysis");
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    if (mode == "web" || mode == "research")
        mode = "web_research";
    if (mode != "code_analysis" && mode != "web_research")
    {
        WriteError(res,
                   400,
                   "only public modes code_analysis and web_research are supported by /v1/agent/debug; generic remains internal-only",
                   "invalid_request_error",
                   "invalid_mode");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const std::string request_id = gen_request_id();
    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = request_id;
    ctx->session_id = body.value("session_id", request_id);
    ctx->model = body.value("model", get_default_model());
    ctx->is_chat = true;
    ctx->stream = false;
    ctx->use_agent = true;
    ctx->agent_mode = mode;
    ctx->agent_debug = body.value("debug", true);
    ctx->agent_include_trace = true;
    ctx->agent_output_format = body.value("agent_output_format", std::string("structured"));
    ctx->agent_max_steps = std::max(1, std::min(body.value("max_steps", 4), 8));
    ctx->messages = {{"user", query}};
    ctx->session = session_mgr_->getOrCreate(ctx->session_id, ctx->model, "");
    ctx->params["max_tokens"] = std::to_string(std::max(64, body.value("max_tokens", 256)));
    if (body.contains("tools") && body["tools"].is_array())
    {
        for (const auto &item : body["tools"])
        {
            if (item.is_string())
                ctx->agent_tools.push_back(item.get<std::string>());
        }
    }

    ctx->on_finish = [this, ctx, start_time](FinishReason r)
    {
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(r, dur_ms);
        LOG(INFO) << "[agent-debug] done req=" << ctx->request_id
                  << " dur_ms=" << dur_ms
                  << " reason=" << finish_reason_to_str(r);
    };

    res.SetOnClose([ctx]
                   {
                       ctx->cancelled.store(true, std::memory_order_release);
                       ctx->EmitFinish(FinishReason::cancelled); });

    bool accepted = session_executor_.Submit(ctx->session, [this, ctx]
                                             {
                                                 if (ctx->use_agent && agent_executor_)
                                                 {
                                                     agent_executor_->Run(ctx);
                                                     return;
                                                 }
                                                 ctx->error_message = "agent executor unavailable";
                                                 ctx->EmitFinish(FinishReason::error);
                                             });
    if (!accepted)
    {
        ctx->error_message = "SessionExecutor: session queue full, session=" + ctx->session_id;
        ctx->params["error_code"] = "overloaded";
        ctx->EmitFinish(FinishReason::error);
    }

    ctx->WaitFinishOrCancel([&res]
                            { return res.IsAlive(); }, std::chrono::milliseconds(100));

    if (!res.IsAlive())
        return;

    if (!ctx->error_message.empty() || ctx->finish_reason == FinishReason::error)
    {
        WriteError(res, 500, ctx->error_message.empty() ? "agent debug failed" : ctx->error_message, "internal_error", "agent_debug_failed");
        return;
    }

    json planner_steps = json::array();
    for (const auto &item : ctx->agent_trace)
        planner_steps.push_back(ToJson(item));

    json evidence = json::array();
    for (const auto &item : ctx->agent_evidence)
        evidence.push_back(ToJson(item));

    json out = {
        {"mode", ctx->agent_mode},
        {"query", query},
        {"planner_steps", planner_steps},
        {"evidence", evidence},
        {"final_answer", ctx->agent_structured_output.empty() ? json{{"content", ctx->final_text}} : ctx->agent_structured_output},
        {"content", ctx->final_text},
        {"usage",
         {{"prompt_tokens", ctx->usage.prompt_tokens},
          {"completion_tokens", ctx->usage.completion_tokens},
          {"total_tokens", ctx->usage.total_tokens}}}};
    if (!ctx->agent_structured_output.empty() && ctx->agent_structured_output.contains("references"))
        out["references"] = ctx->agent_structured_output["references"];
    if (!ctx->agent_structured_output.empty() && ctx->agent_structured_output.contains("subqueries"))
        out["subqueries"] = ctx->agent_structured_output["subqueries"];

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump(-1, ' ', false, json::error_handler_t::replace));
    res.End();
}

void HttpGateway::HandleAdminRagReloadIndex(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    if (!rag_executor_)
    {
        WriteError(res, 503, "rag executor unavailable", "service_unavailable_error", "rag_unavailable");
        return;
    }

    std::string error;
    if (!rag_executor_->Reload(error))
    {
        WriteError(res, 500, error.empty() ? "rag reload failed" : error, "internal_error", "rag_reload_failed");
        return;
    }

    const auto status = rag_executor_->GetStatus();
    json out = {
        {"ok", true},
        {"index_path", status.index_path},
        {"docs_chunk_count", status.docs_chunk_count},
        {"repo_code_chunk_count", status.repo_code_chunk_count},
        {"vector_index_loaded", status.vector_index_loaded},
        {"last_loaded_at", status.last_loaded_at},
    };
    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}

void HttpGateway::HandleAdminRagStatus(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    if (!rag_executor_)
    {
        WriteError(res, 503, "rag executor unavailable", "service_unavailable_error", "rag_unavailable");
        return;
    }

    const auto status = rag_executor_->GetStatus();
    json out = {
        {"index_path", status.index_path},
        {"docs_chunk_count", status.docs_chunk_count},
        {"repo_code_chunk_count", status.repo_code_chunk_count},
        {"vector_index_loaded", status.vector_index_loaded},
        {"last_loaded_at", status.last_loaded_at},
        {"vector_index_path", status.vector_index_path},
        {"chunk_metadata_path", status.chunk_metadata_path},
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

    if (!chat_service_)
    {
        WriteError(res, 500, "chat service unavailable", "internal_error", "chat_service_unavailable");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const ChatExecutionRequest chat_request{parsed.request.ctx, parsed.request.client_messages};
    const ChatError validation_error = chat_service_->ValidateRequest(chat_request);
    if (validation_error.HasError())
    {
        WriteError(res,
                   status_from_chat_error_kind(validation_error.kind),
                   validation_error.message,
                   type_from_chat_error_kind(validation_error.kind),
                   validation_error.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
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

    auto ctx = run_result.ctx;
    const ChatError error = chat_service_->BuildError(ctx);
    if (error.HasError())
    {
        WriteError(res,
                   status_from_chat_error_kind(error.kind),
                   error.message,
                   type_from_chat_error_kind(error.kind),
                   error.code);
        return;
    }

    const ChatResponse response = chat_service_->BuildResponse(ctx);
    json out = build_chat_completion_json(ctx->request_id, response);

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

    if (!chat_service_)
    {
        WriteError(*res_ptr, 500, "chat service unavailable", "internal_error", "chat_service_unavailable");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const ChatExecutionRequest chat_request{parsed.request.ctx, parsed.request.client_messages};
    const ChatError validation_error = chat_service_->ValidateRequest(chat_request);
    if (validation_error.HasError())
    {
        WriteError(*res_ptr,
                   status_from_chat_error_kind(validation_error.kind),
                   validation_error.message,
                   type_from_chat_error_kind(validation_error.kind),
                   validation_error.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    auto ctx = parsed.request.ctx;

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
