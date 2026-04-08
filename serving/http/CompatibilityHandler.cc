#include "HttpGateway.h"

#include "HttpGatewayInternal.h"
#include "http_types.h"
#include "serving/core/ServingContext.h"
#include "utils/json.hpp"

#include <cctype>
#include <algorithm>
#include <chrono>
#include <glog/logging.h>
#include <memory>
#include <string>

using json = nlohmann::json;
using namespace http_gateway_internal;

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

    const std::string request_id = GenRequestId();
    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = request_id;
    ctx->session_id = body.value("session_id", request_id);
    ctx->model = body.value("model", GetDefaultModel());
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
                  << " reason=" << FinishReasonToStr(r);
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
