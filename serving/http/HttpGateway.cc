#include "HttpGateway.h"

#include "http_types.h"
#include "HttpStreamSession.h"
#include "ChatRequestParser.h"
#include "serving/core/ServingContext.h"
#include "serving/core/SessionManager.h"
#include "OpenAIStreamWriter.h"
#include "HttpUtils.h"
#include "serving/service/RequestLogging.h"

#include "../../utils/json.hpp"
#include <glog/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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

    std::string normalize_backend_name(std::string backend)
    {
        std::transform(backend.begin(), backend.end(), backend.begin(), [](unsigned char ch)
                       { return static_cast<char>(std::tolower(ch)); });
        if (backend == "rpc" || backend == "remote" || backend == "worker" || backend == "stackflow")
            return "stackflow";
        if (backend == "local" || backend == "llama")
            return "local";
        return "";
    }

    int64_t parse_int64_or_default(const std::string &value, int64_t default_value = 0)
    {
        if (value.empty())
            return default_value;
        try
        {
            return std::stoll(value);
        }
        catch (...)
        {
            return default_value;
        }
    }

    bool has_deadline(std::chrono::steady_clock::time_point deadline)
    {
        return deadline != std::chrono::steady_clock::time_point::max();
    }

    void start_sync_cancel_watchdog(const std::shared_ptr<std::atomic<bool>> &cancel_flag,
                                    const std::shared_ptr<std::atomic<bool>> &done,
                                    const std::function<bool()> &is_client_alive)
    {
        if (!cancel_flag || !done)
            return;

        std::thread([cancel_flag, done, is_client_alive]
        {
            while (!done->load(std::memory_order_acquire))
            {
                if (!is_client_alive())
                {
                    cancel_flag->store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
    }

    struct ScopedWatchdogDone
    {
        std::shared_ptr<std::atomic<bool>> done;

        ~ScopedWatchdogDone()
        {
            if (done)
                done->store(true, std::memory_order_release);
        }
    };

    PlatformError build_chat_platform_error(const ServingContext &ctx)
    {
        const std::string error_code =
            ctx.params.count("error_code") ? ctx.params.at("error_code") : std::string();

        if (error_code == "invalid_request" ||
            error_code == "model_not_found" ||
            error_code == "capability_not_supported" ||
            error_code == "rag_invalid_request" ||
            error_code == "rag_no_user_query")
        {
            return {
                PlatformErrorKind::InvalidRequest,
                error_code,
                ctx.error_message.empty() ? "invalid chat request" : ctx.error_message};
        }

        if (error_code == "backend_timeout" ||
            error_code == "request_timeout")
        {
            return {
                PlatformErrorKind::Timeout,
                error_code,
                ctx.error_message.empty() ? "request timed out" : ctx.error_message};
        }

        if (error_code == "backend_not_available" ||
            error_code == "rag_index_missing" ||
            error_code == "rag_unavailable")
        {
            return {
                PlatformErrorKind::ServiceUnavailable,
                error_code,
                ctx.error_message.empty() ? "backend unavailable" : ctx.error_message};
        }

        if (error_code == "queue_full" || error_code == "queue_timeout" ||
            ctx.error_message.find("queue full") != std::string::npos)
        {
            return {
                PlatformErrorKind::RateLimit,
                error_code.empty() ? "queue_full" : error_code,
                ctx.error_message.empty() ? "engine overloaded" : ctx.error_message};
        }

        return {
            PlatformErrorKind::Internal,
            error_code.empty() ? "internal_error" : error_code,
            ctx.error_message.empty() ? "engine error" : ctx.error_message};
    }

    struct EmbeddingsRequestParseResult
    {
        bool ok = false;
        int status = 500;
        std::string message;
        std::string type;
        std::string code;
        EmbeddingsRequest request;
    };

    struct RerankRequestParseResult
    {
        bool ok = false;
        int status = 500;
        std::string message;
        std::string type;
        std::string code;
        RerankRequest request;
    };

    EmbeddingsRequestParseResult ParseEmbeddingsRequestBody(const std::string &body_text,
                                                           const std::string &default_model,
                                                           const std::string &request_id)
    {
        EmbeddingsRequestParseResult result;
        result.status = 400;
        result.type = "invalid_request_error";

        json body;
        try
        {
            body = json::parse(body_text.empty() ? "{}" : body_text);
        }
        catch (...)
        {
            result.message = "invalid json";
            result.code = "invalid_json";
            return result;
        }

        if (!body.contains("input"))
        {
            result.message = "input is required";
            result.code = "invalid_input";
            return result;
        }

        if (body["input"].is_string())
        {
            result.request.input.push_back(body["input"].get<std::string>());
        }
        else if (body["input"].is_array())
        {
            for (const auto &item : body["input"])
            {
                if (!item.is_string())
                {
                    result.message = "input array must contain only strings";
                    result.code = "invalid_input";
                    return result;
                }
                result.request.input.push_back(item.get<std::string>());
            }
        }
        else
        {
            result.message = "input must be a string or array of strings";
            result.code = "invalid_input";
            return result;
        }

        result.request.request_id = request_id;
        result.request.model =
            (body.contains("model") && body["model"].is_string())
                ? body["model"].get<std::string>()
                : default_model;

        if (body.contains("encoding_format"))
        {
            if (!body["encoding_format"].is_string())
            {
                result.message = "encoding_format must be string";
                result.code = "invalid_encoding_format";
                return result;
            }
            result.request.encoding_format = body["encoding_format"].get<std::string>();
            std::transform(result.request.encoding_format.begin(),
                           result.request.encoding_format.end(),
                           result.request.encoding_format.begin(),
                           [](unsigned char ch)
                           { return static_cast<char>(std::tolower(ch)); });
        }

        std::string preferred_backend;
        if (body.contains("inference_backend") && body["inference_backend"].is_string())
            preferred_backend = body["inference_backend"].get<std::string>();
        else if (body.contains("backend") && body["backend"].is_string())
            preferred_backend = body["backend"].get<std::string>();
        result.request.inference_backend = normalize_backend_name(std::move(preferred_backend));

        result.ok = true;
        return result;
    }

    RerankRequestParseResult ParseRerankRequestBody(const std::string &body_text,
                                                    const std::string &default_model,
                                                    const std::string &request_id)
    {
        RerankRequestParseResult result;
        result.status = 400;
        result.type = "invalid_request_error";

        json body;
        try
        {
            body = json::parse(body_text.empty() ? "{}" : body_text);
        }
        catch (...)
        {
            result.message = "invalid json";
            result.code = "invalid_json";
            return result;
        }

        if (!body.contains("query") || !body["query"].is_string())
        {
            result.message = "query is required";
            result.code = "invalid_query";
            return result;
        }

        if (!body.contains("documents"))
        {
            result.message = "documents is required";
            result.code = "invalid_documents";
            return result;
        }

        if (!body["documents"].is_array())
        {
            result.message = "documents must be an array of strings";
            result.code = "invalid_documents";
            return result;
        }

        result.request.documents.reserve(body["documents"].size());
        for (const auto &item : body["documents"])
        {
            if (!item.is_string())
            {
                result.message = "documents array must contain only strings";
                result.code = "invalid_documents";
                return result;
            }
            result.request.documents.push_back(item.get<std::string>());
        }

        result.request.request_id = request_id;
        result.request.query = body["query"].get<std::string>();
        result.request.model =
            (body.contains("model") && body["model"].is_string())
                ? body["model"].get<std::string>()
                : default_model;

        if (body.contains("top_n"))
        {
            if (!body["top_n"].is_number_integer())
            {
                result.message = "top_n must be integer";
                result.code = "invalid_top_n";
                return result;
            }
            result.request.top_n = body["top_n"].get<int>();
        }

        std::string preferred_backend;
        if (body.contains("inference_backend") && body["inference_backend"].is_string())
            preferred_backend = body["inference_backend"].get<std::string>();
        else if (body.contains("backend") && body["backend"].is_string())
            preferred_backend = body["backend"].get<std::string>();
        result.request.inference_backend = normalize_backend_name(std::move(preferred_backend));

        result.ok = true;
        return result;
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

    std::string build_experimental_api_disabled_message(const std::string &route,
                                                        const std::string &env_name)
    {
        return route + " is an experimental compatibility API and is disabled by default; set " +
               env_name + "=1 to re-enable it explicitly";
    }

    json build_embeddings_json(const EmbeddingsResponse &response)
    {
        json data = json::array();
        for (const auto &item : response.data)
        {
            data.push_back({
                {"object", "embedding"},
                {"index", item.index},
                {"embedding", item.embedding},
            });
        }

        return {
            {"object", "list"},
            {"data", data},
            {"model", response.model},
            {"usage",
             {{"prompt_tokens", response.usage.prompt_tokens},
              {"total_tokens", response.usage.total_tokens}}}
        };
    }

    json build_rerank_json(const RerankResponse &response)
    {
        json data = json::array();
        for (const auto &item : response.data)
        {
            data.push_back({
                {"object", "rerank_result"},
                {"index", item.index},
                {"document", item.document},
                {"relevance_score", item.relevance_score},
            });
        }

        return {
            {"object", "list"},
            {"data", data},
            {"model", response.model},
            {"usage",
             {{"prompt_tokens", response.usage.prompt_tokens},
              {"total_tokens", response.usage.total_tokens}}}
        };
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
    experimental_agent_api_enabled_ = get_env_bool("EXPERIMENTAL_AGENT_API_ENABLED", false);
    experimental_rag_api_enabled_ = get_env_bool("EXPERIMENTAL_RAG_API_ENABLED", false);
    rag_retrieval_debug_api_enabled_ = get_env_bool("RAG_ENABLE_RETRIEVAL_DEBUG_API", true);
    max_concurrent_requests_ = get_env_int("MAX_CONCURRENT_REQUESTS", 0);
    max_model_concurrency_ = get_env_int("MAX_MODEL_CONCURRENCY", 0);
    max_session_concurrency_ = get_env_int("MAX_SESSION_CONCURRENCY", 0);
    request_timeout_ms_ = get_env_int("HTTP_REQUEST_TIMEOUT_MS", 0);

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
                const auto queue_wait_ms = parse_int64_or_default(
                    ctx.params.count("queue_wait_ms") ? ctx.params.at("queue_wait_ms") : std::string(),
                    0);
                const std::string error_code =
                    ctx.params.count("error_code") ? ctx.params.at("error_code") : std::string();
                const int status_code = reason == FinishReason::cancelled
                                            ? 499
                                            : (reason == FinishReason::error
                                                   ? build_chat_platform_error(ctx).HttpStatus()
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
                                     finish_reason_to_str(reason),
                                     status_code,
                                     error_code,
                                     prompt_tokens,
                                     completion_tokens,
                                     total_tokens,
                                     stream});

    prompt_tokens_total_.fetch_add(prompt_tokens, std::memory_order_relaxed);
    completion_tokens_total_.fetch_add(completion_tokens, std::memory_order_relaxed);
    total_tokens_total_.fetch_add(total_tokens, std::memory_order_relaxed);
    if (error_code == "backend_timeout" || error_code == "request_timeout")
        timeout_requests_.fetch_add(1, std::memory_order_relaxed);
    if (error_code == "queue_full" || error_code == "queue_timeout" ||
        error_code == "rate_limit_global" || error_code == "rate_limit_model" ||
        error_code == "rate_limit_session")
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
        if (error_code == "backend_timeout" || error_code == "request_timeout")
        {
            stats.requests_timeout_total += 1;
            stats.timeout_total += 1;
        }
        if (error_code == "queue_full" || error_code == "queue_timeout" ||
            error_code == "rate_limit_global" || error_code == "rate_limit_model" ||
            error_code == "rate_limit_session")
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

void HttpGateway::HandleHealth(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = health_service_.BuildHealth(BuildPlatformRuntimeSnapshot());

    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("X-EdgeLLM-Compat-Route", "/healthz");
    res.SetHeader("X-EdgeLLM-Route-Status", "compatibility-alias");
    res.SetHeader("Connection", "close");
    res.Write(out.dump());
    res.End();
}

void HttpGateway::HandleHealthz(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = health_service_.BuildHealth(BuildPlatformRuntimeSnapshot());

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
        {"requests_timeout_total", timeout_requests_.load(std::memory_order_relaxed)},
        {"requests_rate_limited_total", rate_limited_requests_.load(std::memory_order_relaxed)},
        {"prompt_tokens_total", prompt_tokens_total_.load(std::memory_order_relaxed)},
        {"completion_tokens_total", completion_tokens_total_.load(std::memory_order_relaxed)},
        {"total_tokens_total", total_tokens_total_.load(std::memory_order_relaxed)},
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

void HttpGateway::WriteExperimentalApiDisabled(HttpResponse &res,
                                               const std::string &route,
                                               const std::string &env_name)
{
    WriteError(res,
               404,
               build_experimental_api_disabled_message(route, env_name),
               "invalid_request_error",
               "experimental_api_disabled");
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

void HttpGateway::HandleEmbeddings(const HttpRequest &req, HttpResponse &res)
{
    const auto start_time = std::chrono::steady_clock::now();
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    in_flight_.fetch_add(1, std::memory_order_relaxed);

    const std::string request_id = gen_request_id();
    auto parsed = ParseEmbeddingsRequestBody(req.body, get_default_model(), request_id);
    if (!parsed.ok)
    {
        WriteError(res, parsed.status, parsed.message, parsed.type, parsed.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              ModelCapability::Embeddings,
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
    LogPlatformRequest("start", RequestLogRecord{
                                    parsed.request.request_id,
                                    "/v1/embeddings",
                                    parsed.request.model,
                                    parsed.request.inference_backend,
                                    std::string(ToString(parsed.request.capability)),
                                    "",
                                    -1,
                                    -1,
                                    "",
                                    0,
                                    "",
                                    0,
                                    0,
                                    0,
                                    false});

    if (!embeddings_service_)
    {
        const PlatformError error{
            PlatformErrorKind::Internal,
            "embeddings_service_unavailable",
            "embeddings service unavailable"};
        WriteError(res, error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
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

    const EmbeddingsError validation_error = embeddings_service_->ValidateRequest(parsed.request);
    if (validation_error.HasError())
    {
        WriteError(res, validation_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
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
    const PlatformError limit_error = AcquireRequestLease(parsed.request.model, "", request_lease);
    if (limit_error.HasError())
    {
        WriteError(res, limit_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
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

    parsed.request.cancelled = std::make_shared<std::atomic<bool>>(false);
    if (request_timeout_ms_ > 0)
        parsed.request.deadline = start_time + std::chrono::milliseconds(request_timeout_ms_);
    const auto watchdog_done = std::make_shared<std::atomic<bool>>(false);
    ScopedWatchdogDone stop_watchdog{watchdog_done};
    start_sync_cancel_watchdog(parsed.request.cancelled, watchdog_done, [&res]()
    {
        return res.IsAlive();
    });

    const auto result = embeddings_service_->Run(parsed.request);
    if (!res.IsAlive())
    {
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                              parsed.request.capability,
                              "",
                              FinishReason::cancelled,
                              499,
                              "request_cancelled",
                              0,
                              dur_ms,
                              result.response.usage.prompt_tokens,
                              result.response.usage.completion_tokens,
                              result.response.usage.total_tokens,
                              false);
        RecordFinish(FinishReason::cancelled, dur_ms);
        return;
    }
    if (result.error.HasError())
    {
        WriteError(res, result.error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                              parsed.request.capability,
                              "",
                              FinishReason::error,
                              result.error.HttpStatus(),
                              result.error.code,
                              0,
                              dur_ms,
                              result.response.usage.prompt_tokens,
                              result.response.usage.completion_tokens,
                              result.response.usage.total_tokens,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const json out = build_embeddings_json(result.response);
    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump(-1, ' ', false, json::error_handler_t::replace));
    res.End();

    const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time)
                            .count();
    RecordGovernedRequest(parsed.request.request_id,
                          "/v1/embeddings",
                          result.response.model,
                          result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                          parsed.request.capability,
                          "",
                          FinishReason::stop,
                          200,
                          "",
                          0,
                          dur_ms,
                          result.response.usage.prompt_tokens,
                          result.response.usage.completion_tokens,
                          result.response.usage.total_tokens,
                          false);
    RecordFinish(FinishReason::stop, dur_ms);
}

void HttpGateway::HandleRerank(const HttpRequest &req, HttpResponse &res)
{
    const auto start_time = std::chrono::steady_clock::now();
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    in_flight_.fetch_add(1, std::memory_order_relaxed);

    const std::string request_id = gen_request_id();
    auto parsed = ParseRerankRequestBody(req.body, get_default_model(), request_id);
    if (!parsed.ok)
    {
        WriteError(res, parsed.status, parsed.message, parsed.type, parsed.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(request_id,
                              "/v1/rerank",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              ModelCapability::Rerank,
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
    LogPlatformRequest("start", RequestLogRecord{
                                    parsed.request.request_id,
                                    "/v1/rerank",
                                    parsed.request.model,
                                    parsed.request.inference_backend,
                                    std::string(ToString(parsed.request.capability)),
                                    "",
                                    -1,
                                    -1,
                                    "",
                                    0,
                                    "",
                                    0,
                                    0,
                                    0,
                                    false});

    if (!rerank_service_)
    {
        const PlatformError error{
            PlatformErrorKind::Internal,
            "rerank_service_unavailable",
            "rerank service unavailable"};
        WriteError(res, error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/rerank",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
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

    const RerankError validation_error = rerank_service_->ValidateRequest(parsed.request);
    if (validation_error.HasError())
    {
        WriteError(res, validation_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/rerank",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
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
    const PlatformError limit_error = AcquireRequestLease(parsed.request.model, "", request_lease);
    if (limit_error.HasError())
    {
        WriteError(res, limit_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/rerank",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
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

    parsed.request.cancelled = std::make_shared<std::atomic<bool>>(false);
    if (request_timeout_ms_ > 0)
        parsed.request.deadline = start_time + std::chrono::milliseconds(request_timeout_ms_);
    const auto watchdog_done = std::make_shared<std::atomic<bool>>(false);
    ScopedWatchdogDone stop_watchdog{watchdog_done};
    start_sync_cancel_watchdog(parsed.request.cancelled, watchdog_done, [&res]()
    {
        return res.IsAlive();
    });

    const auto result = rerank_service_->Run(parsed.request);
    if (!res.IsAlive())
    {
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/rerank",
                              parsed.request.model,
                              result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                              parsed.request.capability,
                              "",
                              FinishReason::cancelled,
                              499,
                              "request_cancelled",
                              0,
                              dur_ms,
                              result.response.usage.prompt_tokens,
                              result.response.usage.completion_tokens,
                              result.response.usage.total_tokens,
                              false);
        RecordFinish(FinishReason::cancelled, dur_ms);
        return;
    }
    if (result.error.HasError())
    {
        WriteError(res, result.error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/rerank",
                              parsed.request.model,
                              result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                              parsed.request.capability,
                              "",
                              FinishReason::error,
                              result.error.HttpStatus(),
                              result.error.code,
                              0,
                              dur_ms,
                              result.response.usage.prompt_tokens,
                              result.response.usage.completion_tokens,
                              result.response.usage.total_tokens,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const json out = build_rerank_json(result.response);
    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump(-1, ' ', false, json::error_handler_t::replace));
    res.End();

    const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time)
                            .count();
    RecordGovernedRequest(parsed.request.request_id,
                          "/v1/rerank",
                          result.response.model,
                          result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                          parsed.request.capability,
                          "",
                          FinishReason::stop,
                          200,
                          "",
                          0,
                          dur_ms,
                          result.response.usage.prompt_tokens,
                          result.response.usage.completion_tokens,
                          result.response.usage.total_tokens,
                          false);
    RecordFinish(FinishReason::stop, dur_ms);
}

void HttpGateway::HandleAdminModelsStatus(const HttpRequest &req, HttpResponse &res)
{
    (void)req;
    const json out = admin_status_service_.BuildModelsStatus(model_catalog_service_.ListModels(),
                                                             BuildBackendRuntimeSnapshots());

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
        BuildBackendRuntimeSnapshots(),
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
    if (!experimental_rag_api_enabled_)
    {
        WriteExperimentalApiDisabled(res, "/v1/retrieval/search", "EXPERIMENTAL_RAG_API_ENABLED");
        return;
    }
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

    if (!experimental_agent_api_enabled_)
    {
        WriteExperimentalApiDisabled(res, "/v1/agent/debug", "EXPERIMENTAL_AGENT_API_ENABLED");
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordFinish(FinishReason::error, dur_ms);
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
    if (!experimental_rag_api_enabled_)
    {
        WriteExperimentalApiDisabled(res, "/admin/rag/reload-index", "EXPERIMENTAL_RAG_API_ENABLED");
        return;
    }
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
    if (!experimental_rag_api_enabled_)
    {
        WriteExperimentalApiDisabled(res, "/admin/rag/status", "EXPERIMENTAL_RAG_API_ENABLED");
        return;
    }
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
    if (has_deadline(ctx->deadline))
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
    if (has_deadline(ctx->deadline))
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
