#include "serving/rag/PromptAssembler.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::string build_hit_block(size_t ordinal, const RetrievalHit &hit)
{
    std::ostringstream oss;
    oss << "[" << ordinal << "] "
        << "kb=" << hit.chunk.kb_name
        << " path=" << hit.chunk.path;
    if (hit.chunk.start_line > 0 || hit.chunk.end_line > 0)
    {
        oss << " lines=" << hit.chunk.start_line << "-" << hit.chunk.end_line;
    }
    if (!hit.chunk.symbol.empty())
    {
        oss << " symbol=" << hit.chunk.symbol;
    }
    if (!hit.chunk.title.empty())
    {
        oss << " title=" << hit.chunk.title;
    }
    oss << "\n" << hit.chunk.text << "\n";
    return oss.str();
}
} // namespace

PromptAssembler::PromptAssembler(size_t max_context_chars)
    : max_context_chars_(max_context_chars)
{
}

PromptAssembler::Result PromptAssembler::Assemble(const std::vector<Message> &messages,
                                                 const std::vector<RetrievalHit> &hits) const
{
    Result result;
    result.messages = messages;

    std::ostringstream ctx;
    ctx << "You must answer using only the retrieved repository context below. "
        << "If the context is insufficient, explicitly say you do not know.\n\n"
        << "Retrieved context:\n";

    size_t used = 0;
    size_t ordinal = 1;
    for (const auto &hit : hits)
    {
        std::string block = build_hit_block(ordinal, hit);
        if (max_context_chars_ > 0 && used > 0 && used + block.size() > max_context_chars_)
            break;

        if (max_context_chars_ > 0 && used == 0 && block.size() > max_context_chars_)
        {
            block.resize(max_context_chars_);
        }

        used += block.size();
        ctx << block;
        ++ordinal;
    }

    if (hits.empty())
    {
        ctx << "[no-context]\nNo relevant retrieved context was found for this question.\n";
    }

    result.injected_chars = used;
    result.messages.insert(result.messages.begin(), {"system", ctx.str()});
    return result;
}
