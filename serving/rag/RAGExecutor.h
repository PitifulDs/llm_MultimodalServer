#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "serving/core/ServingContext.h"
#include "serving/rag/PromptAssembler.h"
#include "serving/rag/Retriever.h"
#include "serving/rag/SqliteIndexStore.h"
#include "serving/rag/vector/EmbeddingProvider.h"
#include "serving/rag/vector/FaissIndexStore.h"

class RAGExecutor
{
public:
    struct Options
    {
        std::string index_path;
        std::string vector_index_path;
        std::string chunk_metadata_path;
        std::string vector_embeddings_path;
        std::string vector_id_map_path;
        int default_top_k = 6;
        size_t max_context_chars = 6000;
        std::string default_mode = "lexical";
        std::string default_fusion = "rrf";
        bool enable_neighbor_expand = true;
        int max_neighbor_count = 1;
    };

    struct Status
    {
        std::string index_path;
        int docs_chunk_count = 0;
        int repo_code_chunk_count = 0;
        bool vector_index_loaded = false;
        std::string last_loaded_at;
        std::string vector_index_path;
        std::string chunk_metadata_path;
    };

    explicit RAGExecutor(Options opt);

    bool Apply(const std::shared_ptr<ServingContext> &ctx);
    bool Search(const RetrievalRequest &request,
                RetrievalResponse &response_out,
                std::string &error_out);
    bool OpenChunk(const std::string &chunk_id,
                   RagChunk &chunk_out,
                   std::string &error_out);
    bool Reload(std::string &error_out);
    Status GetStatus() const;

private:
    static std::string ExtractLastUserQuery(const std::vector<Message> &messages);
    RetrievalRequest BuildRequest(const RagOptions &options, const std::string &query) const;

private:
    Options opt_;
    SqliteIndexStore store_;
    EmbeddingProvider embedding_provider_;
    FaissIndexStore vector_store_;
    Retriever retriever_;
    PromptAssembler assembler_;
    mutable std::mutex mu_;
};
