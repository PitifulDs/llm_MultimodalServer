#pragma once

#include <string>
#include <vector>

#include "serving/rag/Chunk.h"
#include "serving/rag/SqliteIndexStore.h"
#include "serving/rag/vector/EmbeddingProvider.h"
#include "serving/rag/vector/FaissIndexStore.h"

struct RetrievalRequest
{
    std::string kb;
    std::string query;
    int top_k = 0;
    std::string mode;
    int lexical_top_k = 0;
    int vector_top_k = 0;
    std::string fusion;
    bool debug = false;
    bool enable_neighbor_expand = true;
    int max_neighbor_count = 1;
};

struct RetrievalResponse
{
    std::vector<RetrievalHit> hits;
    std::string normalized_query;
    RagRetrievalSummary summary;
};

class Retriever
{
public:
    Retriever(const SqliteIndexStore &store,
              const FaissIndexStore *vector_store,
              EmbeddingProvider embedding_provider);

    bool Retrieve(const RetrievalRequest &request,
                  RetrievalResponse &response_out,
                  std::string &error_out) const;

    bool OpenChunk(const std::string &chunk_id,
                   RagChunk &chunk_out,
                   std::string &error_out) const;

    static std::string NormalizeQuery(const std::string &query);

private:
    const SqliteIndexStore &store_;
    const FaissIndexStore *vector_store_ = nullptr;
    EmbeddingProvider embedding_provider_;
};
