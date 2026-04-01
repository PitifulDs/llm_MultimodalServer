#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "serving/rag/Chunk.h"
#include "serving/rag/vector/VectorTypes.h"

class FaissIndexStore
{
public:
    struct Options
    {
        std::string index_path;
        std::string metadata_path;
        std::string embeddings_path;
        std::string id_map_path;
    };

    explicit FaissIndexStore(Options options = {});

    bool Reload(std::string &error_out);
    bool Search(const std::string &kb,
                const std::vector<float> &query_embedding,
                int top_k,
                std::vector<VectorSearchHit> &hits_out,
                std::string &error_out) const;
    bool GetChunkById(const std::string &chunk_id, RagChunk &chunk_out) const;
    VectorIndexStatus status() const;

private:
    bool load_chunk_metadata(std::string &error_out);
    bool load_id_map(std::vector<std::string> &ids_out, std::string &error_out) const;
    bool load_embeddings(size_t expected_rows, std::string &error_out);

private:
    Options options_;
    VectorIndexStatus status_;
    std::vector<float> embeddings_;
    std::vector<std::string> row_to_chunk_id_;
    std::unordered_map<std::string, size_t> chunk_id_to_row_;
    std::unordered_map<std::string, RagChunk> chunks_;
};
