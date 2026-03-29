#pragma once

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
};

struct RetrievalHit
{
    RagChunk chunk;
    double lexical_score = 0.0;
    double final_score = 0.0;
};

struct RagOptions
{
    bool enabled = false;
    std::string kb;
    int top_k = 0;
    std::string mode;
    bool return_references = false;
};

struct RagReference
{
    std::string kb;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    double score = 0.0;
};
