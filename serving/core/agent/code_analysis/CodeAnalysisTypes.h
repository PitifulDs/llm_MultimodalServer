#pragma once

#include <string>
#include <vector>

#include "utils/json.hpp"

enum class CodeAnalysisQuestionType
{
    module_responsibility,
    call_chain,
    symbol_behavior,
    location_lookup,
    config_interface,
    troubleshooting,
    unknown
};

struct CodeEvidence
{
    std::string source_type;
    std::string reference_source;
    std::string chunk_id;
    std::string path;
    std::string title;
    std::string url;
    int start_line = 0;
    int end_line = 0;
    std::string symbol;
    std::string snippet;
    std::string why_relevant;
    double score = 0.0;
};

struct AgentTraceStep
{
    int step_index = 0;
    std::string selected_tool;
    nlohmann::json tool_input = nlohmann::json::object();
    std::string tool_output_summary;
    size_t evidence_count = 0;
    std::string planner_reason;
};

struct CodeAnalysisPlanStep
{
    std::string tool_name;
    nlohmann::json tool_input = nlohmann::json::object();
    std::string planner_reason;
    bool deterministic = true;
};

struct CodeAnalysisFinalAnswer
{
    std::string summary;
    std::vector<std::string> analysis;
    std::vector<CodeEvidence> evidence;
    std::vector<std::string> risks;
    std::vector<std::string> next_steps;
};

struct CodeAnalysisSynthesis
{
    std::string summary;
    std::vector<std::string> analysis;
    std::vector<std::string> risks;
    std::vector<std::string> next_steps;
};

struct CodeAnalysisQuestionHints
{
    std::vector<std::string> symbols;
    std::vector<std::string> file_paths;
    std::vector<std::string> keywords;
    std::string primary_symbol;
    std::string primary_file_path;
    std::string primary_directory;
};

struct ParsedSearchKbHit
{
    std::string chunk_id;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string symbol;
    std::string snippet;
    double score = 0.0;
};

struct ParsedOpenChunk
{
    std::string chunk_id;
    std::string kb_name;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string symbol;
    std::string text;
};

struct ParsedSearchCodeMatch
{
    std::string path;
    int line = 0;
    int score = 0;
    std::string symbol;
    std::string text;
};

struct ParsedFileExcerpt
{
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string snippet;
};

struct ParsedDocMatch
{
    std::string path;
    int score = 0;
    std::string snippet;
};

struct ParsedWebSearchHit
{
    std::string title;
    std::string url;
    std::string snippet;
};

struct ParsedFetchedUrl
{
    std::string title;
    std::string url;
    std::string canonical_url;
    std::string text;
};

inline std::string ToString(CodeAnalysisQuestionType type)
{
    switch (type)
    {
    case CodeAnalysisQuestionType::module_responsibility:
        return "module_responsibility";
    case CodeAnalysisQuestionType::call_chain:
        return "call_chain";
    case CodeAnalysisQuestionType::symbol_behavior:
        return "symbol_behavior";
    case CodeAnalysisQuestionType::location_lookup:
        return "location_lookup";
    case CodeAnalysisQuestionType::config_interface:
        return "config_interface";
    case CodeAnalysisQuestionType::troubleshooting:
        return "troubleshooting";
    case CodeAnalysisQuestionType::unknown:
    default:
        return "unknown";
    }
}

inline nlohmann::json ToJson(const CodeEvidence &evidence)
{
    return {
        {"source_type", evidence.source_type},
        {"reference_source", evidence.reference_source},
        {"chunk_id", evidence.chunk_id},
        {"path", evidence.path},
        {"title", evidence.title},
        {"url", evidence.url},
        {"start_line", evidence.start_line},
        {"end_line", evidence.end_line},
        {"symbol", evidence.symbol},
        {"snippet", evidence.snippet},
        {"why_relevant", evidence.why_relevant},
        {"score", evidence.score},
    };
}

inline nlohmann::json ToJson(const AgentTraceStep &step)
{
    return {
        {"step_index", step.step_index},
        {"selected_tool", step.selected_tool},
        {"tool_input", step.tool_input},
        {"tool_output_summary", step.tool_output_summary},
        {"evidence_count", step.evidence_count},
        {"planner_reason", step.planner_reason},
    };
}

inline nlohmann::json ToJson(const CodeAnalysisFinalAnswer &answer)
{
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto &item : answer.evidence)
        evidence.push_back(ToJson(item));

    return {
        {"summary", answer.summary},
        {"analysis", answer.analysis},
        {"evidence", evidence},
        {"risks", answer.risks},
        {"next_steps", answer.next_steps},
    };
}
