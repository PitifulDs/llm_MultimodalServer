#include "serving/rag/RAGExecutor.h"

#include <chrono>
#include <utility>

#include <glog/logging.h>

RAGExecutor::RAGExecutor(Options opt)
    : opt_(std::move(opt)),
      store_(opt_.index_path),
      retriever_(store_),
      assembler_(opt_.max_context_chars)
{
}

std::string RAGExecutor::ExtractLastUserQuery(const std::vector<Message> &messages)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        if (it->role == "user" && !it->content.empty())
            return it->content;
    }
    return {};
}

bool RAGExecutor::Apply(const std::shared_ptr<ServingContext> &ctx) const
{
    if (!ctx || !ctx->rag_options.enabled)
        return true;

    const auto start = std::chrono::steady_clock::now();
    const std::string query = ExtractLastUserQuery(ctx->messages);
    if (query.empty())
    {
        ctx->params["error_code"] = "rag_no_user_query";
        ctx->error_message = "RAG requires a non-empty user message";
        return false;
    }

    RagOptions effective = ctx->rag_options;
    if (effective.top_k <= 0)
        effective.top_k = opt_.default_top_k;
    if (effective.mode.empty())
        effective.mode = "lexical";

    std::vector<RetrievalHit> hits;
    std::string normalized_query;
    std::string error;
    if (!retriever_.Retrieve(effective.kb,
                             query,
                             effective.top_k,
                             effective.mode,
                             hits,
                             normalized_query,
                             error))
    {
        if (error.find("not found:") != std::string::npos)
            ctx->params["error_code"] = "rag_index_missing";
        else if (error.rfind("rag.", 0) == 0)
            ctx->params["error_code"] = "rag_invalid_request";
        else
            ctx->params["error_code"] = "rag_search_failed";
        ctx->error_message = error;
        return false;
    }

    ctx->rag_options = effective;
    ctx->rag_hits = hits;

    const auto assembled = assembler_.Assemble(ctx->messages, hits);
    ctx->messages = assembled.messages;

    const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
    LOG(INFO) << "[rag] req=" << ctx->request_id
              << " kb=" << effective.kb
              << " top_k=" << effective.top_k
              << " hits=" << hits.size()
              << " latency_ms=" << latency_ms
              << " injected_chars=" << assembled.injected_chars
              << " query=" << normalized_query;
    return true;
}
