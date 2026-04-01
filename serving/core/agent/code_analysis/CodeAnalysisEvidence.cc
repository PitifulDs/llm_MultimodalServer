#include "serving/core/agent/code_analysis/CodeAnalysisEvidence.h"

#include <algorithm>

#include "serving/core/agent/code_analysis/CodeAnalysisHeuristics.h"

namespace
{
std::string choose_symbol(const std::string &left, const std::string &right)
{
    return right.empty() ? left : right;
}
} // namespace

size_t CodeAnalysisEvidenceStore::AddToolOutput(const std::string &tool_name,
                                                const nlohmann::json &tool_input,
                                                const std::string &tool_output,
                                                const std::string &question,
                                                CodeAnalysisQuestionType question_type)
{
    (void)tool_input;
    size_t added = 0;

    if (tool_name == "search_kb")
    {
        for (const auto &hit : ParseSearchKbHits(tool_output))
        {
            CodeEvidence evidence;
            evidence.source_type = "kb";
            evidence.chunk_id = hit.chunk_id;
            evidence.path = hit.path;
            evidence.start_line = hit.start_line;
            evidence.end_line = hit.end_line;
            evidence.symbol = hit.symbol;
            evidence.snippet = hit.snippet;
            evidence.score = hit.score;
            evidence.why_relevant = BuildWhyRelevant(tool_name, question, question_type, hit.symbol, hit.path);
            added += AddEvidence(std::move(evidence));
        }
        return added;
    }

    if (tool_name == "open_chunk")
    {
        ParsedOpenChunk chunk;
        if (ParseOpenChunkOutput(tool_output, chunk))
        {
            CodeEvidence evidence;
            evidence.source_type = "kb";
            evidence.chunk_id = chunk.chunk_id;
            evidence.path = chunk.path;
            evidence.start_line = chunk.start_line;
            evidence.end_line = chunk.end_line;
            evidence.symbol = chunk.symbol;
            evidence.snippet = TrimCodeAnalysisText(chunk.text, 260);
            evidence.score = 1.0;
            evidence.why_relevant = BuildWhyRelevant(tool_name, question, question_type, chunk.symbol, chunk.path);
            added += AddEvidence(std::move(evidence));
        }
        return added;
    }

    if (tool_name == "search_code")
    {
        for (const auto &match : ParseSearchCodeMatches(tool_output))
        {
            CodeEvidence evidence;
            evidence.source_type = "search_code";
            evidence.path = match.path;
            evidence.start_line = match.line;
            evidence.end_line = match.line;
            evidence.symbol = match.symbol;
            evidence.snippet = match.text;
            evidence.score = static_cast<double>(match.score);
            evidence.why_relevant = BuildWhyRelevant(tool_name, question, question_type, match.symbol, match.path);
            added += AddEvidence(std::move(evidence));
        }
        return added;
    }

    if (tool_name == "read_file")
    {
        ParsedFileExcerpt excerpt;
        if (ParseReadFileOutput(tool_output, excerpt))
        {
            CodeEvidence evidence;
            evidence.source_type = "read_file";
            evidence.path = excerpt.path;
            evidence.start_line = excerpt.start_line;
            evidence.end_line = excerpt.end_line;
            evidence.snippet = excerpt.snippet;
            evidence.score = 1.0;
            evidence.why_relevant = BuildWhyRelevant(tool_name, question, question_type, "", excerpt.path);
            added += AddEvidence(std::move(evidence));
        }
        return added;
    }

    if (tool_name == "search_docs" || tool_name == "get_config" || tool_name == "get_server_status")
    {
        const auto docs = ParseDocMatches(tool_output);
        if (!docs.empty())
        {
            for (const auto &match : docs)
            {
                CodeEvidence evidence;
                evidence.source_type = "docs";
                evidence.path = match.path;
                evidence.snippet = match.snippet;
                evidence.score = static_cast<double>(match.score);
                evidence.why_relevant = BuildWhyRelevant(tool_name, question, question_type, "", match.path);
                added += AddEvidence(std::move(evidence));
            }
            return added;
        }

        CodeEvidence evidence;
        evidence.source_type = tool_name == "get_config" ? "config" : "docs";
        evidence.path = tool_name == "get_server_status" ? "runtime" : "";
        evidence.snippet = TrimCodeAnalysisText(tool_output, 260);
        evidence.score = 1.0;
        evidence.why_relevant = BuildWhyRelevant(tool_name, question, question_type, "", evidence.path);
        added += AddEvidence(std::move(evidence));
    }

    return added;
}

bool CodeAnalysisEvidenceStore::Empty() const
{
    return evidence_.empty();
}

size_t CodeAnalysisEvidenceStore::Size() const
{
    return evidence_.size();
}

bool CodeAnalysisEvidenceStore::HasSourceType(const std::string &source_type) const
{
    return std::any_of(evidence_.begin(), evidence_.end(), [&](const CodeEvidence &item)
                       { return item.source_type == source_type; });
}

bool CodeAnalysisEvidenceStore::HasFocusedContext() const
{
    return HasSourceType("read_file") || HasSourceType("kb");
}

const std::vector<CodeEvidence> &CodeAnalysisEvidenceStore::All() const
{
    return evidence_;
}

std::vector<CodeEvidence> CodeAnalysisEvidenceStore::Top(size_t limit) const
{
    std::vector<CodeEvidence> out = evidence_;
    std::sort(out.begin(), out.end(), [](const CodeEvidence &left, const CodeEvidence &right)
              {
                  if (left.score != right.score)
                      return left.score > right.score;
                  if (left.path != right.path)
                      return left.path < right.path;
                  return left.start_line < right.start_line;
              });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

size_t CodeAnalysisEvidenceStore::AddEvidence(CodeEvidence evidence)
{
    for (auto &existing : evidence_)
    {
        const bool same_chunk = !evidence.chunk_id.empty() && existing.chunk_id == evidence.chunk_id;
        const bool same_range = !evidence.path.empty() &&
                                existing.path == evidence.path &&
                                existing.start_line == evidence.start_line &&
                                existing.end_line == evidence.end_line;
        const bool same_symbol = !evidence.symbol.empty() && existing.symbol == evidence.symbol;
        if (!same_chunk && !same_range && !same_symbol)
            continue;

        if (existing.snippet.size() < evidence.snippet.size())
            existing.snippet = std::move(evidence.snippet);
        if (existing.why_relevant.empty())
            existing.why_relevant = std::move(evidence.why_relevant);
        existing.score = std::max(existing.score, evidence.score);
        existing.symbol = choose_symbol(existing.symbol, evidence.symbol);
        existing.path = choose_symbol(existing.path, evidence.path);
        if (existing.start_line == 0)
            existing.start_line = evidence.start_line;
        if (existing.end_line == 0)
            existing.end_line = evidence.end_line;
        return 0;
    }

    evidence_.push_back(std::move(evidence));
    return 1;
}

std::string CodeAnalysisEvidenceStore::BuildWhyRelevant(const std::string &tool_name,
                                                        const std::string &question,
                                                        CodeAnalysisQuestionType question_type,
                                                        const std::string &symbol,
                                                        const std::string &path) const
{
    (void)question;
    switch (question_type)
    {
    case CodeAnalysisQuestionType::call_chain:
        return "这段证据可用于判断调用链或依赖关系。";
    case CodeAnalysisQuestionType::module_responsibility:
        return "这段证据直接描述了模块职责或入口位置。";
    case CodeAnalysisQuestionType::symbol_behavior:
        if (!symbol.empty())
            return "这里命中了目标符号 `" + symbol + "` 的实现或上下文。";
        return "这里提供了函数或类行为的实现上下文。";
    case CodeAnalysisQuestionType::config_interface:
        return "这里包含接口、配置或流式输出相关实现。";
    case CodeAnalysisQuestionType::location_lookup:
        return "这里给出了用户要找的位置或文件。";
    case CodeAnalysisQuestionType::troubleshooting:
        return "这里可用于解释潜在异常路径或边界条件。";
    case CodeAnalysisQuestionType::unknown:
    default:
        break;
    }

    if (tool_name == "search_docs")
        return "这是文档侧证据，可辅助定位实现。";
    if (!path.empty())
        return "这里与问题直接相关。";
    return "这是与问题相关的辅助证据。";
}
