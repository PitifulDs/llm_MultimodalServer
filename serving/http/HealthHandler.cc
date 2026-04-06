#include "HttpGateway.h"

#include "http_types.h"
#include "utils/json.hpp"

using json = nlohmann::json;

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
