#pragma once

#include <memory>
#include <string>

#include "serving/core/ServingContext.h"
#include "serving/rag/PromptAssembler.h"
#include "serving/rag/Retriever.h"
#include "serving/rag/SqliteIndexStore.h"

class RAGExecutor
{
public:
    struct Options
    {
        std::string index_path;
        int default_top_k = 6;
        size_t max_context_chars = 6000;
    };

    explicit RAGExecutor(Options opt);

    bool Apply(const std::shared_ptr<ServingContext> &ctx) const;

private:
    static std::string ExtractLastUserQuery(const std::vector<Message> &messages);

private:
    Options opt_;
    SqliteIndexStore store_;
    Retriever retriever_;
    PromptAssembler assembler_;
};
