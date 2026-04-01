#include "serving/rag/Retriever.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <future>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr int kRrfConstant = 60;
constexpr int kMaxSearchK = 60;

std::string to_lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return s;
}

std::vector<std::string> split_query_terms(const std::string &query)
{
    std::vector<std::string> terms;
    std::string current;
    for (unsigned char ch : query)
    {
        if (std::isalnum(ch) != 0 || ch == '_' || ch == '/' || ch == ':' || ch == '.' || ch == '-')
        {
            current.push_back(static_cast<char>(std::tolower(ch)));
            continue;
        }
        if (!current.empty())
        {
            terms.push_back(current);
            current.clear();
        }
    }
    if (!current.empty())
        terms.push_back(current);
    return terms;
}

struct QuerySignals
{
    std::string raw_lower;
    std::string normalized_lower;
    std::vector<std::string> terms;
    std::vector<std::string> path_terms;
    std::vector<std::string> symbol_terms;
    std::vector<std::string> dir_terms;
    bool mentions_vendor = false;
    bool mentions_tests = false;
    bool mentions_docs = false;
};

bool has_any_substring(const std::string &text, const std::vector<std::string> &needles)
{
    for (const auto &needle : needles)
    {
        if (!needle.empty() && text.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

QuerySignals build_query_signals(const std::string &raw_query, const std::string &normalized_query)
{
    QuerySignals signals;
    signals.raw_lower = to_lower_copy(raw_query);
    signals.normalized_lower = to_lower_copy(normalized_query);
    signals.terms = split_query_terms(raw_query);
    for (const auto &term : signals.terms)
    {
        if (term.find('/') != std::string::npos || term.find('.') != std::string::npos)
            signals.path_terms.push_back(term);
        if (term.find("::") != std::string::npos || term.find('_') != std::string::npos)
            signals.symbol_terms.push_back(term);
        if (term.find('/') != std::string::npos)
        {
            std::filesystem::path p(term);
            for (const auto &part : p.parent_path())
            {
                const std::string piece = to_lower_copy(part.string());
                if (piece.size() >= 2)
                    signals.dir_terms.push_back(piece);
            }
        }
    }
    signals.mentions_vendor = has_any_substring(signals.raw_lower, {"thirds", "vendor", "dependency", "llama.cpp", "ggml", "sycl"});
    signals.mentions_tests = has_any_substring(signals.raw_lower, {"test", "tests", "node/test", "unittest", "smoke"});
    signals.mentions_docs = has_any_substring(signals.raw_lower, {"readme", "docs", "文档", "说明"});
    return signals;
}

double compute_boost(const RetrievalHit &hit, const QuerySignals &signals)
{
    double boost = 0.0;
    const std::string symbol = to_lower_copy(hit.chunk.symbol);
    const std::string path = to_lower_copy(hit.chunk.path);
    const std::string filename = to_lower_copy(std::filesystem::path(hit.chunk.path).filename().string());

    if (!symbol.empty())
    {
        if (signals.raw_lower == symbol || signals.normalized_lower == symbol)
            boost += 0.85;
        else
        {
            for (const auto &term : signals.symbol_terms)
            {
                if (term == symbol || term.rfind(symbol + "::", 0) == 0 || term.find(symbol) != std::string::npos)
                {
                    boost += 0.7;
                    break;
                }
            }
        }
    }

    for (const auto &term : signals.path_terms)
    {
        if (term == filename || path.find(term) != std::string::npos)
        {
            boost += 0.9;
            break;
        }
    }

    const std::string parent = to_lower_copy(std::filesystem::path(hit.chunk.path).parent_path().string());
    for (const auto &dir_term : signals.dir_terms)
    {
        if (!dir_term.empty() && parent.find(dir_term) != std::string::npos)
            boost += 0.2;
    }

    const bool is_primary_project_path =
        path.rfind("serving/", 0) == 0 ||
        path.rfind("tools/", 0) == 0 ||
        path.rfind("scripts/", 0) == 0;
    const bool is_docs_path =
        path == "readme.md" ||
        path.rfind("docs/", 0) == 0;
    const bool is_vendor_path =
        path.rfind("thirds/", 0) == 0 ||
        path.rfind("build/", 0) == 0;
    const bool is_test_path =
        path.rfind("tests/", 0) == 0 ||
        path.rfind("node/test/", 0) == 0;

    if (is_primary_project_path)
        boost += 0.35;
    if (is_docs_path)
        boost += signals.mentions_docs ? 0.35 : 0.1;
    if (is_vendor_path)
        boost += signals.mentions_vendor ? 0.0 : -1.15;
    if (is_test_path)
        boost += signals.mentions_tests ? 0.05 : -0.7;
    if (hit.chunk.kb_name == "repo_code" && hit.chunk.language == "markdown" && !signals.mentions_docs)
        boost -= 0.25;

    return boost;
}

double normalized_score(double value, double best)
{
    if (best <= 0.0 || value <= 0.0)
        return 0.0;
    return std::min(1.0, value / best);
}

bool hit_compare(const RetrievalHit &a, const RetrievalHit &b)
{
    if (a.final_score != b.final_score)
        return a.final_score > b.final_score;
    if (a.vector_score != b.vector_score)
        return a.vector_score > b.vector_score;
    if (a.lexical_score != b.lexical_score)
        return a.lexical_score > b.lexical_score;
    if (a.chunk.path != b.chunk.path)
        return a.chunk.path < b.chunk.path;
    return a.chunk.start_line < b.chunk.start_line;
}
} // namespace

Retriever::Retriever(const SqliteIndexStore &store,
                     const FaissIndexStore *vector_store,
                     EmbeddingProvider embedding_provider)
    : store_(store),
      vector_store_(vector_store),
      embedding_provider_(std::move(embedding_provider))
{
}

std::string Retriever::NormalizeQuery(const std::string &query)
{
    std::string out;
    out.reserve(query.size());

    bool seen_non_space = false;
    bool pending_space = false;
    for (unsigned char ch : query)
    {
        if (std::isspace(ch) || std::ispunct(ch))
        {
            if (seen_non_space)
                pending_space = true;
            continue;
        }

        if (pending_space && !out.empty())
            out.push_back(' ');
        out.push_back(static_cast<char>(ch));
        seen_non_space = true;
        pending_space = false;
    }

    return out;
}

bool Retriever::Retrieve(const RetrievalRequest &request,
                         RetrievalResponse &response_out,
                         std::string &error_out) const
{
    response_out = RetrievalResponse{};
    error_out.clear();
    response_out.normalized_query = NormalizeQuery(request.query);
    response_out.summary.normalized_query = response_out.normalized_query;

    if (request.kb != "docs" && request.kb != "repo_code")
    {
        error_out = "rag.kb must be one of: docs, repo_code";
        return false;
    }

    const std::string mode = request.mode.empty() ? "lexical" : request.mode;
    if (mode != "lexical" && mode != "vector" && mode != "hybrid")
    {
        error_out = "rag.mode must be one of: lexical, vector, hybrid";
        return false;
    }
    const std::string fusion = request.fusion.empty() ? "rrf" : request.fusion;
    if (fusion != "rrf" && fusion != "weighted_sum")
    {
        error_out = "rag.fusion must be one of: rrf, weighted_sum";
        return false;
    }

    response_out.summary.mode = mode;
    response_out.summary.fusion = fusion;

    const int top_k = std::clamp(request.top_k <= 0 ? 6 : request.top_k, 1, 20);
    const int lexical_top_k = std::clamp(request.lexical_top_k > 0 ? request.lexical_top_k : top_k, 1, kMaxSearchK);
    const int vector_top_k = std::clamp(request.vector_top_k > 0 ? request.vector_top_k : top_k, 1, kMaxSearchK);
    const QuerySignals signals = build_query_signals(request.query, response_out.normalized_query);

    std::vector<RetrievalHit> lexical_hits;
    std::vector<VectorSearchHit> vector_hits;
    std::string lexical_error;
    std::string vector_error;
    bool lexical_ok = true;
    bool vector_ok = true;

    const auto retrieval_start = std::chrono::steady_clock::now();

    std::future<void> lexical_future;
    if (mode == "lexical" || mode == "hybrid")
    {
        lexical_future = std::async(std::launch::async, [&]()
        {
            const auto start = std::chrono::steady_clock::now();
            lexical_ok = store_.Search(request.kb,
                                       response_out.normalized_query,
                                       std::min(kMaxSearchK, lexical_top_k * 4),
                                       lexical_hits,
                                       lexical_error);
            response_out.summary.lexical_search_latency_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        });
    }

    std::future<void> vector_future;
    if (mode == "vector" || mode == "hybrid")
    {
        vector_future = std::async(std::launch::async, [&]()
        {
            const auto start = std::chrono::steady_clock::now();
            if (!vector_store_ || !vector_store_->status().loaded)
            {
                vector_ok = false;
                vector_error = "vector index is not loaded";
            }
            else
            {
                const auto query_embedding = embedding_provider_.Embed(request.query);
                vector_ok = vector_store_->Search(request.kb,
                                                  query_embedding,
                                                  std::min(kMaxSearchK, vector_top_k * 4),
                                                  vector_hits,
                                                  vector_error);
            }
            response_out.summary.vector_search_latency_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        });
    }

    if (lexical_future.valid())
        lexical_future.get();
    if (vector_future.valid())
        vector_future.get();

    if ((mode == "lexical" || mode == "hybrid") && !lexical_ok)
    {
        error_out = lexical_error;
        return false;
    }
    if (mode == "vector" && !vector_ok)
    {
        error_out = vector_error;
        return false;
    }

    response_out.summary.lexical_hit_count = static_cast<int>(lexical_hits.size());
    response_out.summary.vector_hit_count = static_cast<int>(vector_hits.size());

    std::unordered_map<std::string, RetrievalHit> merged;
    double max_lexical = 0.0;
    for (size_t i = 0; i < lexical_hits.size(); ++i)
    {
        lexical_hits[i].lexical_rank = static_cast<int>(i + 1);
        max_lexical = std::max(max_lexical, lexical_hits[i].lexical_score);
        auto &slot = merged[lexical_hits[i].chunk.chunk_id];
        slot.chunk = lexical_hits[i].chunk;
        slot.lexical_score = lexical_hits[i].lexical_score;
        slot.lexical_rank = lexical_hits[i].lexical_rank;
    }

    double max_vector = 0.0;
    for (size_t i = 0; i < vector_hits.size(); ++i)
    {
        max_vector = std::max(max_vector, vector_hits[i].score);
        auto &slot = merged[vector_hits[i].chunk_id];
        if (slot.chunk.chunk_id.empty())
        {
            RagChunk chunk;
            std::string chunk_error;
            if (!OpenChunk(vector_hits[i].chunk_id, chunk, chunk_error))
                continue;
            slot.chunk = std::move(chunk);
        }
        slot.vector_score = vector_hits[i].score;
        slot.vector_rank = static_cast<int>(i + 1);
    }

    std::vector<RetrievalHit> ranked;
    ranked.reserve(merged.size());
    for (auto &entry : merged)
    {
        RetrievalHit hit = std::move(entry.second);
        double fused = 0.0;
        if (mode == "lexical")
        {
            fused = hit.lexical_score;
        }
        else if (mode == "vector")
        {
            fused = hit.vector_score;
        }
        else if (fusion == "weighted_sum")
        {
            fused = 0.55 * normalized_score(hit.lexical_score, max_lexical) +
                    0.45 * normalized_score(hit.vector_score, max_vector);
        }
        else
        {
            if (hit.lexical_rank > 0)
                fused += 1.0 / static_cast<double>(kRrfConstant + hit.lexical_rank);
            if (hit.vector_rank > 0)
                fused += 1.0 / static_cast<double>(kRrfConstant + hit.vector_rank);
        }

        hit.final_score = fused + compute_boost(hit, signals);
        ranked.push_back(std::move(hit));
    }

    std::sort(ranked.begin(), ranked.end(), hit_compare);

    std::vector<RetrievalHit> selected;
    selected.reserve(static_cast<size_t>(top_k));
    std::set<std::string> selected_ids;
    std::set<std::string> selected_paths;
    std::set<std::string> selected_symbols;

    auto try_push = [&](const RetrievalHit &hit, bool strict_diversity) -> bool
    {
        if (selected_ids.find(hit.chunk.chunk_id) != selected_ids.end())
            return false;
        if (strict_diversity)
        {
            if (selected_paths.find(hit.chunk.path) != selected_paths.end())
                return false;
            if (!hit.chunk.symbol.empty() && selected_symbols.find(hit.chunk.symbol) != selected_symbols.end())
                return false;
        }
        selected.push_back(hit);
        selected_ids.insert(hit.chunk.chunk_id);
        selected_paths.insert(hit.chunk.path);
        if (!hit.chunk.symbol.empty())
            selected_symbols.insert(hit.chunk.symbol);
        return true;
    };

    for (const auto &hit : ranked)
    {
        if (selected.size() >= static_cast<size_t>(top_k))
            break;
        try_push(hit, true);
    }
    for (const auto &hit : ranked)
    {
        if (selected.size() >= static_cast<size_t>(top_k))
            break;
        try_push(hit, false);
    }

    if (request.enable_neighbor_expand && request.max_neighbor_count > 0 && selected.size() < static_cast<size_t>(top_k))
    {
        const auto seeds = selected;
        for (const auto &seed : seeds)
        {
            int added = 0;
            for (const auto &neighbor_id : {seed.chunk.prev_chunk_id, seed.chunk.next_chunk_id})
            {
                if (added >= request.max_neighbor_count || selected.size() >= static_cast<size_t>(top_k))
                    break;
                if (neighbor_id.empty() || selected_ids.find(neighbor_id) != selected_ids.end())
                    continue;

                RagChunk neighbor_chunk;
                std::string chunk_error;
                if (!OpenChunk(neighbor_id, neighbor_chunk, chunk_error))
                    continue;
                if (neighbor_chunk.kb_name != request.kb)
                    continue;

                RetrievalHit neighbor = seed;
                neighbor.chunk = std::move(neighbor_chunk);
                neighbor.lexical_score = 0.0;
                neighbor.vector_score = 0.0;
                neighbor.final_score = std::max(0.0, seed.final_score * 0.92);
                neighbor.lexical_rank = 0;
                neighbor.vector_rank = 0;
                neighbor.from_neighbor = true;
                if (try_push(neighbor, false))
                    ++added;
            }
        }
    }

    std::sort(selected.begin(), selected.end(), hit_compare);
    if (selected.size() > static_cast<size_t>(top_k))
        selected.resize(static_cast<size_t>(top_k));

    response_out.hits = std::move(selected);
    response_out.summary.final_hit_count = static_cast<int>(response_out.hits.size());
    response_out.summary.references_returned = response_out.summary.final_hit_count;
    response_out.summary.retrieval_latency_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - retrieval_start).count();
    return true;
}

bool Retriever::OpenChunk(const std::string &chunk_id,
                          RagChunk &chunk_out,
                          std::string &error_out) const
{
    error_out.clear();
    if (store_.GetChunkById(chunk_id, chunk_out, error_out))
        return true;

    const std::string sqlite_error = error_out;
    error_out.clear();
    if (vector_store_ && vector_store_->GetChunkById(chunk_id, chunk_out))
        return true;

    error_out = !sqlite_error.empty() ? sqlite_error : ("chunk not found: " + chunk_id);
    return false;
}
