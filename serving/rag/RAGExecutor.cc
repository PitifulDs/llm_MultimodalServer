#include "serving/rag/RAGExecutor.h"

#include "utils/json.hpp"

#include <chrono>
#include <utility>

#include <glog/logging.h>

using json = nlohmann::json;

namespace
{
std::string pick_default(const std::string &value, const std::string &fallback)
{
    return value.empty() ? fallback : value;
}
} // namespace

RAGExecutor::RAGExecutor(Options opt)
    : opt_(std::move(opt)),
      store_(opt_.index_path),
      embedding_provider_(),
      vector_store_({opt_.vector_index_path, opt_.chunk_metadata_path, opt_.vector_embeddings_path, opt_.vector_id_map_path}),
      retriever_(store_, &vector_store_, embedding_provider_),
      assembler_(opt_.max_context_chars)
{
    std::string ignored_error;
    vector_store_.Reload(ignored_error);
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

RetrievalRequest RAGExecutor::BuildRequest(const RagOptions &options, const std::string &query) const
{
    RetrievalRequest request;
    request.kb = options.kb;
    request.query = query;
    request.top_k = options.top_k > 0 ? options.top_k : opt_.default_top_k;
    request.mode = pick_default(options.mode, opt_.default_mode);
    request.lexical_top_k = options.lexical_top_k;
    request.vector_top_k = options.vector_top_k;
    request.fusion = pick_default(options.fusion, opt_.default_fusion);
    request.debug = options.debug;
    request.enable_neighbor_expand = opt_.enable_neighbor_expand;
    request.max_neighbor_count = opt_.max_neighbor_count;
    return request;
}

bool RAGExecutor::Apply(const std::shared_ptr<ServingContext> &ctx)
{
    if (!ctx || !ctx->rag_options.enabled)
        return true;

    const bool request_stream = ctx->stream;
    const std::string query = ExtractLastUserQuery(ctx->messages);
    if (query.empty())
    {
        ctx->params["error_code"] = "rag_no_user_query";
        ctx->error_message = "RAG requires a non-empty user message";
        return false;
    }

    RagOptions effective = ctx->rag_options;
    effective.top_k = effective.top_k > 0 ? effective.top_k : opt_.default_top_k;
    effective.mode = pick_default(effective.mode, opt_.default_mode);
    effective.fusion = pick_default(effective.fusion, opt_.default_fusion);

    RetrievalResponse retrieval;
    std::string error;
    if (!Search(BuildRequest(effective, query), retrieval, error))
    {
        if (error.find("not found:") != std::string::npos ||
            error.find("index is not loaded") != std::string::npos)
        {
            ctx->params["error_code"] = "rag_index_missing";
        }
        else if (error.rfind("rag.", 0) == 0)
        {
            ctx->params["error_code"] = "rag_invalid_request";
        }
        else
        {
            ctx->params["error_code"] = "rag_search_failed";
        }
        ctx->error_message = error;
        return false;
    }

    ctx->rag_options = effective;
    ctx->rag_hits = retrieval.hits;
    ctx->rag_summary = retrieval.summary;

    const auto assembled = assembler_.Assemble(ctx->messages, retrieval.hits);
    ctx->messages = assembled.messages;
    ctx->rag_summary.injected_chars = static_cast<int>(assembled.injected_chars);
    ctx->stream = request_stream;

    if (request_stream)
    {
        json metadata = {
            {"references", json::array()},
            {"retrieval", {
                              {"normalized_query", retrieval.normalized_query},
                              {"mode", retrieval.summary.mode},
                              {"fusion", retrieval.summary.fusion},
                              {"lexical_hit_count", retrieval.summary.lexical_hit_count},
                              {"vector_hit_count", retrieval.summary.vector_hit_count},
                              {"final_hit_count", retrieval.summary.final_hit_count},
                              {"injected_chars", retrieval.summary.injected_chars},
                          }}};
        for (const auto &hit : retrieval.hits)
        {
            metadata["references"].push_back({
                {"chunk_id", hit.chunk.chunk_id},
                {"kb", hit.chunk.kb_name},
                {"path", hit.chunk.path},
                {"symbol", hit.chunk.symbol},
                {"start_line", hit.chunk.start_line},
                {"end_line", hit.chunk.end_line},
                {"score", hit.final_score},
            });
        }
        ctx->stream_metadata_json = metadata.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    LOG(INFO) << "[rag] req=" << ctx->request_id
              << " kb=" << effective.kb
              << " mode=" << effective.mode
              << " top_k=" << effective.top_k
              << " lexical_hit_count=" << retrieval.summary.lexical_hit_count
              << " vector_hit_count=" << retrieval.summary.vector_hit_count
              << " fusion=" << retrieval.summary.fusion
              << " references_returned=" << retrieval.summary.references_returned
              << " latency_ms=" << retrieval.summary.retrieval_latency_ms
              << " injected_chars=" << assembled.injected_chars
              << " query=" << retrieval.normalized_query;
    return true;
}

bool RAGExecutor::Search(const RetrievalRequest &request,
                         RetrievalResponse &response_out,
                         std::string &error_out)
{
    std::lock_guard<std::mutex> lk(mu_);
    return retriever_.Retrieve(request, response_out, error_out);
}

bool RAGExecutor::OpenChunk(const std::string &chunk_id,
                            RagChunk &chunk_out,
                            std::string &error_out)
{
    std::lock_guard<std::mutex> lk(mu_);
    return retriever_.OpenChunk(chunk_id, chunk_out, error_out);
}

bool RAGExecutor::Reload(std::string &error_out)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (opt_.chunk_metadata_path.empty() || opt_.vector_embeddings_path.empty() || opt_.vector_id_map_path.empty())
    {
        error_out.clear();
        return true;
    }
    return vector_store_.Reload(error_out);
}

RAGExecutor::Status RAGExecutor::GetStatus() const
{
    std::lock_guard<std::mutex> lk(mu_);
    Status status;
    status.index_path = opt_.index_path;
    status.vector_index_path = opt_.vector_index_path;
    status.chunk_metadata_path = opt_.chunk_metadata_path;

    std::string ignored_error;
    status.docs_chunk_count = store_.CountChunks("docs", ignored_error);
    status.repo_code_chunk_count = store_.CountChunks("repo_code", ignored_error);

    const auto vector_status = vector_store_.status();
    status.vector_index_loaded = vector_status.loaded;
    status.last_loaded_at = vector_status.last_loaded_at;
    return status;
}
