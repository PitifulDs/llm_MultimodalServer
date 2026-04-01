#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RagChunk
{
    std::string chunk_id;
    std::string kb_name;
    std::string doc_id;
    std::string path;
    std::string title;
    std::string symbol;
    int start_line = 0;
    int end_line = 0;
    std::string language;
    std::string text;
    int token_estimate = 0;
    std::string prev_chunk_id;
    std::string next_chunk_id;
};

struct RetrievalHit
{
    RagChunk chunk;
    double lexical_score = 0.0;
    double vector_score = 0.0;
    double final_score = 0.0;
    int lexical_rank = 0;
    int vector_rank = 0;
    bool from_neighbor = false;
};

struct RagOptions
{
    bool enabled = false;
    std::string kb;
    int top_k = 0;
    std::string mode;
    int lexical_top_k = 0;
    int vector_top_k = 0;
    std::string fusion;
    bool debug = false;
    bool return_references = false;
};

struct RagReference
{
    std::string kb;
    std::string chunk_id;
    std::string path;
    std::string symbol;
    int start_line = 0;
    int end_line = 0;
    double score = 0.0;
};

struct RagRetrievalSummary
{
    std::string normalized_query;
    std::string mode;
    std::string fusion;
    int lexical_hit_count = 0;
    int vector_hit_count = 0;
    int final_hit_count = 0;
    int references_returned = 0;
    int injected_chars = 0;
    int64_t retrieval_latency_ms = 0;
    int64_t lexical_search_latency_ms = 0;
    int64_t vector_search_latency_ms = 0;
};
