#pragma once

#include <cstddef>
#include <vector>

#include "serving/core/ServingContext.h"
#include "serving/rag/Chunk.h"

class PromptAssembler
{
public:
    struct Result
    {
        std::vector<Message> messages;
        size_t injected_chars = 0;
    };

    explicit PromptAssembler(size_t max_context_chars);

    Result Assemble(const std::vector<Message> &messages,
                    const std::vector<RetrievalHit> &hits) const;

private:
    size_t max_context_chars_ = 0;
};
