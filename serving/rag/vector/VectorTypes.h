#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct VectorSearchHit
{
    std::string chunk_id;
    double score = 0.0;
};

struct VectorIndexStatus
{
    bool loaded = false;
    std::string index_path;
    std::string metadata_path;
    std::string embeddings_path;
    std::string id_map_path;
    std::string last_loaded_at;
    size_t dimension = 0;
    size_t total_chunks = 0;
    size_t docs_chunks = 0;
    size_t repo_code_chunks = 0;
};
