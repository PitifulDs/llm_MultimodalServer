#include "serving/core/agent/code_analysis/CodeAnalysisHeuristics.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace
{
std::string trim_copy(std::string s)
{
    auto is_space = [](unsigned char ch)
    {
        return std::isspace(ch) != 0;
    };

    while (!s.empty() && is_space(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string to_lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return s;
}

std::string trim_token(std::string token)
{
    auto ok = [](unsigned char ch)
    {
        return std::isalnum(ch) != 0 || ch == '_' || ch == ':' || ch == '/' || ch == '.' || ch == '-';
    };
    while (!token.empty() && !ok(static_cast<unsigned char>(token.front())))
        token.erase(token.begin());
    while (!token.empty() && !ok(static_cast<unsigned char>(token.back())))
        token.pop_back();
    return token;
}

bool contains_any(const std::string &text, const std::vector<std::string> &terms)
{
    for (const auto &term : terms)
    {
        if (!term.empty() && text.find(term) != std::string::npos)
            return true;
    }
    return false;
}

bool looks_like_symbol(const std::string &token)
{
    if (token.find("::") != std::string::npos)
        return true;
    for (unsigned char ch : token)
    {
        if (std::isupper(ch) != 0 || ch == '_')
            return true;
    }
    return false;
}

bool looks_like_file_path(const std::string &token)
{
    if (token.rfind("/v1/", 0) == 0 || token.rfind("/admin/", 0) == 0)
        return false;
    const bool has_repo_prefix =
        token.rfind("serving/", 0) == 0 ||
        token.rfind("docs/", 0) == 0 ||
        token.rfind("engine/", 0) == 0 ||
        token.rfind("tests/", 0) == 0 ||
        token.rfind("scripts/", 0) == 0 ||
        token.rfind("tools/", 0) == 0;
    return has_repo_prefix ||
           token.find(".cc") != std::string::npos ||
           token.find(".cpp") != std::string::npos ||
           token.find(".h") != std::string::npos ||
           token.find(".hpp") != std::string::npos ||
           token == "CMakeLists.txt";
}

void dedupe_push(std::vector<std::string> &items, const std::string &value)
{
    if (value.empty())
        return;
    if (std::find(items.begin(), items.end(), value) == items.end())
        items.push_back(value);
}
} // namespace

CodeAnalysisQuestionType ClassifyCodeAnalysisQuestion(const std::string &question)
{
    const std::string lower = to_lower_copy(question);
    const bool asks_location = contains_any(lower, {"在哪里", "哪一层", "哪一行", "哪个文件", "where", "locate", "在哪"});
    if (contains_any(lower, {"调用链", "调用关系", "依赖关系", "谁调用", "谁会调用", "怎么进入", "进入", "经过", "链路", "主链路", "关系", "call chain", "caller", "callee", "dependency"}))
        return CodeAnalysisQuestionType::call_chain;
    if (contains_any(lower, {"做什么", "干什么", "what does", "what is", "函数", "类"}))
        return CodeAnalysisQuestionType::symbol_behavior;
    if (asks_location &&
        contains_any(lower, {"references"}) &&
        !contains_any(lower, {"配置", "参数", "环境变量", "接口", "api", "endpoint"}))
    {
        return CodeAnalysisQuestionType::location_lookup;
    }
    if (contains_any(lower, {"配置", "参数", "环境变量", "接口", "api", "endpoint", "stream", "references"}))
        return CodeAnalysisQuestionType::config_interface;
    if (contains_any(lower, {"职责", "作用", "负责", "模块", "module responsibility", "what is this module for"}))
        return CodeAnalysisQuestionType::module_responsibility;
    if (asks_location)
        return CodeAnalysisQuestionType::location_lookup;
    if (contains_any(lower, {"报错", "问题", "排查", "排障", "why failed", "troubleshoot", "debug"}))
        return CodeAnalysisQuestionType::troubleshooting;
    return CodeAnalysisQuestionType::unknown;
}

CodeAnalysisQuestionHints ExtractCodeAnalysisHints(const std::string &question)
{
    CodeAnalysisQuestionHints hints;
    std::istringstream iss(question);
    std::string token;
    while (iss >> token)
    {
        token = trim_token(token);
        if (token.empty())
            continue;
        if (looks_like_symbol(token))
            dedupe_push(hints.symbols, token);
        if (looks_like_file_path(token))
            dedupe_push(hints.file_paths, token);
        dedupe_push(hints.keywords, token);
    }

    if (!hints.symbols.empty())
        hints.primary_symbol = hints.symbols.front();
    if (!hints.file_paths.empty())
    {
        hints.primary_file_path = hints.file_paths.front();
        const auto pos = hints.primary_file_path.rfind('/');
        if (pos != std::string::npos)
            hints.primary_directory = hints.primary_file_path.substr(0, pos);
    }

    return hints;
}

std::vector<ParsedSearchKbHit> ParseSearchKbHits(const std::string &tool_output)
{
    std::vector<ParsedSearchKbHit> hits;
    std::istringstream iss(tool_output);
    std::string line;
    ParsedSearchKbHit current;
    bool has_current = false;
    while (std::getline(iss, line))
    {
        if (line.rfind("- chunk_id=", 0) == 0)
        {
            if (has_current)
                hits.push_back(current);

            current = ParsedSearchKbHit{};
            has_current = true;

            const auto path_pos = line.find(" path=");
            const auto symbol_pos = line.find(" symbol=");
            const auto score_pos = line.find(" score=");
            if (path_pos == std::string::npos || symbol_pos == std::string::npos)
                continue;

            current.chunk_id = line.substr(std::string("- chunk_id=").size(),
                                           path_pos - std::string("- chunk_id=").size());

            const std::string path_part = line.substr(path_pos + 6, symbol_pos - (path_pos + 6));
            const auto colon = path_part.rfind(':');
            const auto dash = path_part.rfind('-');
            if (colon != std::string::npos && dash != std::string::npos && dash > colon)
            {
                current.path = path_part.substr(0, colon);
                try
                {
                    current.start_line = std::stoi(path_part.substr(colon + 1, dash - colon - 1));
                    current.end_line = std::stoi(path_part.substr(dash + 1));
                }
                catch (...)
                {
                    current.start_line = 0;
                    current.end_line = 0;
                }
            }
            if (score_pos == std::string::npos)
                current.symbol = line.substr(symbol_pos + 8);
            else
            {
                current.symbol = line.substr(symbol_pos + 8, score_pos - (symbol_pos + 8));
                try
                {
                    current.score = std::stod(line.substr(score_pos + 7));
                }
                catch (...)
                {
                    current.score = 0.0;
                }
            }
            continue;
        }

        if (has_current && line.find("snippet=") != std::string::npos)
        {
            current.snippet = trim_copy(line.substr(line.find("snippet=") + 8));
        }
    }

    if (has_current)
        hits.push_back(current);
    return hits;
}

bool ParseOpenChunkOutput(const std::string &tool_output, ParsedOpenChunk &out)
{
    std::istringstream iss(tool_output);
    std::string first_line;
    std::string second_line;
    if (!std::getline(iss, first_line) || !std::getline(iss, second_line))
        return false;
    if (first_line.rfind("CHUNK ", 0) != 0)
        return false;

    out = ParsedOpenChunk{};
    out.chunk_id = trim_copy(first_line.substr(6));

    const auto kb_pos = second_line.find("kb=");
    const auto path_pos = second_line.find(" path=");
    const auto lines_pos = second_line.find(" lines ");
    const auto symbol_pos = second_line.find(" symbol=");
    if (kb_pos != std::string::npos && path_pos != std::string::npos)
        out.kb_name = second_line.substr(kb_pos + 3, path_pos - (kb_pos + 3));
    if (path_pos != std::string::npos && lines_pos != std::string::npos)
        out.path = second_line.substr(path_pos + 6, lines_pos - (path_pos + 6));
    if (lines_pos != std::string::npos)
    {
        const size_t range_begin = lines_pos + 7;
        const size_t range_end = symbol_pos == std::string::npos ? second_line.size() : symbol_pos;
        const std::string range = second_line.substr(range_begin, range_end - range_begin);
        const auto dash = range.find('-');
        if (dash != std::string::npos)
        {
            try
            {
                out.start_line = std::stoi(range.substr(0, dash));
                out.end_line = std::stoi(range.substr(dash + 1));
            }
            catch (...)
            {
                out.start_line = 0;
                out.end_line = 0;
            }
        }
    }
    if (symbol_pos != std::string::npos)
        out.symbol = second_line.substr(symbol_pos + 8);

    std::ostringstream text;
    std::string line;
    bool first = true;
    while (std::getline(iss, line))
    {
        if (!first)
            text << "\n";
        text << line;
        first = false;
    }
    out.text = text.str();
    return true;
}

std::vector<ParsedSearchCodeMatch> ParseSearchCodeMatches(const std::string &tool_output)
{
    std::vector<ParsedSearchCodeMatch> matches;
    std::istringstream iss(tool_output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.rfind("- file=", 0) != 0)
            continue;

        const auto score_pos = line.find(" score=");
        const auto symbol_pos = line.find(" symbol=");
        const auto text_pos = line.find(" text=");
        const std::string file_part = line.substr(7, score_pos == std::string::npos ? std::string::npos : score_pos - 7);
        const auto colon = file_part.rfind(':');
        if (colon == std::string::npos)
            continue;

        ParsedSearchCodeMatch match;
        match.path = file_part.substr(0, colon);
        try
        {
            match.line = std::stoi(file_part.substr(colon + 1));
            if (score_pos != std::string::npos)
            {
                const size_t score_begin = score_pos + 7;
                const size_t score_end = symbol_pos != std::string::npos ? symbol_pos : (text_pos == std::string::npos ? line.size() : text_pos);
                match.score = std::stoi(line.substr(score_begin, score_end - score_begin));
            }
        }
        catch (...)
        {
            continue;
        }
        if (text_pos != std::string::npos)
            match.text = line.substr(text_pos + 6);
        if (symbol_pos != std::string::npos)
        {
            const size_t symbol_begin = symbol_pos + 8;
            const size_t symbol_end = text_pos == std::string::npos ? line.size() : text_pos;
            if (symbol_end > symbol_begin)
                match.symbol = line.substr(symbol_begin, symbol_end - symbol_begin);
        }
        matches.push_back(match);
    }
    return matches;
}

bool ParseReadFileOutput(const std::string &tool_output, ParsedFileExcerpt &out)
{
    std::istringstream iss(tool_output);
    std::string header;
    if (!std::getline(iss, header))
        return false;
    if (header.rfind("FILE ", 0) != 0)
        return false;

    const auto lines_pos = header.find(" lines ");
    if (lines_pos == std::string::npos)
        return false;

    out = ParsedFileExcerpt{};
    out.path = header.substr(5, lines_pos - 5);
    const std::string range = header.substr(lines_pos + 7);
    const auto dash = range.find('-');
    if (dash != std::string::npos)
    {
        try
        {
            out.start_line = std::stoi(range.substr(0, dash));
            out.end_line = std::stoi(range.substr(dash + 1));
        }
        catch (...)
        {
            out.start_line = 0;
            out.end_line = 0;
        }
    }

    std::ostringstream snippet;
    std::string line;
    int count = 0;
    while (std::getline(iss, line) && count < 6)
    {
        if (count > 0)
            snippet << "\n";
        snippet << line;
        ++count;
    }
    out.snippet = snippet.str();
    return true;
}

std::vector<ParsedDocMatch> ParseDocMatches(const std::string &tool_output)
{
    std::vector<ParsedDocMatch> matches;
    std::istringstream iss(tool_output);
    std::string line;
    ParsedDocMatch current;
    bool has_current = false;
    while (std::getline(iss, line))
    {
        if (line.rfind("- file=", 0) == 0)
        {
            if (has_current)
                matches.push_back(current);
            current = ParsedDocMatch{};
            has_current = true;

            const auto score_pos = line.find(" score=");
            if (score_pos == std::string::npos)
                continue;
            current.path = line.substr(7, score_pos - 7);
            try
            {
                current.score = std::stoi(line.substr(score_pos + 7));
            }
            catch (...)
            {
                current.score = 0;
            }
            continue;
        }
        if (has_current && line.find("snippet=") != std::string::npos)
            current.snippet = trim_copy(line.substr(line.find("snippet=") + 8));
    }
    if (has_current)
        matches.push_back(current);
    return matches;
}

bool ToolOutputLooksEmpty(const std::string &tool_output)
{
    const std::string lower = to_lower_copy(tool_output);
    return lower.empty() ||
           lower.find("no code match") != std::string::npos ||
           lower.find("no documentation match") != std::string::npos ||
           lower.find("requires") != std::string::npos ||
           lower.find("not found") != std::string::npos ||
           lower.find("unavailable") != std::string::npos;
}

std::string TrimCodeAnalysisText(const std::string &text, size_t max_chars)
{
    if (text.size() <= max_chars)
        return text;
    if (max_chars <= 32)
        return text.substr(0, max_chars);
    return text.substr(0, max_chars) + "...";
}

std::string BuildPrimarySearchQuery(const std::string &question,
                                    const CodeAnalysisQuestionHints &hints)
{
    const std::string lower = to_lower_copy(question);
    if (lower.find("/v1/retrieval/search") != std::string::npos)
        return "HandleRetrievalSearch";
    if (lower.find("/v1/agent/debug") != std::string::npos)
        return "HandleAgentDebug";
    if (lower.find("stream metadata") != std::string::npos)
        return "OpenAIStreamWriter OnChunk stream_metadata_json metadata_json";
    if (lower.find("references") != std::string::npos && lower.find("哪里") != std::string::npos)
        return "HandleChatCompletion build_rag_references references";
    if (lower.find("search_kb") != std::string::npos && lower.find("open_chunk") != std::string::npos)
        return "RunCodeAnalysis NextStep search_kb open_chunk";
    if (lower.find("默认工具链") != std::string::npos)
        return "default_tools_for_mode";
    if (lower.find("question type") != std::string::npos || lower.find("分类规则") != std::string::npos)
        return "ClassifyCodeAnalysisQuestion";
    if (lower.find("evidence dedup") != std::string::npos || lower.find("evidence dedup") != std::string::npos || lower.find("去重") != std::string::npos)
        return "AddEvidence CodeAnalysisEvidenceStore";
    if (lower.find("structured final answer") != std::string::npos || lower.find("结构化") != std::string::npos)
        return "CodeAnalysisFormatter ToStructuredText Build";
    if (lower.find("planner") != std::string::npos && lower.find("strategy") != std::string::npos)
        return "CodeAnalysisPlanner NextStep PickInitialStep search_kb deterministic";
    if (lower.find("在哪里解析") != std::string::npos)
        return "ParseChatRequestBody agent_debug agent_output_format";
    if (lower.find("挂到响应") != std::string::npos)
        return "HandleChatCompletion agent_trace";
    if (lower.find("search_code 工具") != std::string::npos)
        return "search_code_tool";
    if (lower.find("read_file 工具") != std::string::npos)
        return "read_file_tool";
    if (lower.find("list_files 工具") != std::string::npos)
        return "list_files_tool";
    if (lower.find("open_chunk") != std::string::npos && lower.find("http") != std::string::npos)
        return "open_chunk_handler HttpGateway";
    if (lower.find("hybrid") != std::string::npos && lower.find("rag") != std::string::npos)
        return "RAGExecutor Apply Search hybrid";
    if (lower.find("进入 agentexecutor") != std::string::npos || lower.find("怎么进入") != std::string::npos)
        return "HandleChatCompletion agent_executor_->Run AgentExecutor";
    if (lower.find("servingcontext") != std::string::npos && lower.find("agent evidence") != std::string::npos)
        return "ServingContext.h agent_trace agent_evidence";

    if (hints.symbols.size() >= 2)
        return hints.symbols[0] + " " + hints.symbols[1];
    if (!hints.primary_symbol.empty() && !hints.primary_file_path.empty())
        return hints.primary_symbol + " " + hints.primary_file_path;
    if (!hints.primary_symbol.empty())
        return hints.primary_symbol;
    if (!hints.primary_file_path.empty())
        return hints.primary_file_path;
    if (hints.keywords.size() >= 3)
        return hints.keywords[0] + " " + hints.keywords[1] + " " + hints.keywords[2];
    if (!hints.keywords.empty())
        return hints.keywords.front();
    return TrimCodeAnalysisText(question, 160);
}

std::string InferPreferredSearchPath(const std::string &question,
                                     const CodeAnalysisQuestionHints &hints)
{
    if (!hints.primary_directory.empty())
        return hints.primary_directory;

    const std::string lower = to_lower_copy(question);
    if (lower.find("servingcontext") != std::string::npos)
        return "serving/core/ServingContext.h";
    if (lower.find("httpgateway") != std::string::npos ||
        lower.find("chatrequestparser") != std::string::npos ||
        lower.find("/v1/") != std::string::npos ||
        lower.find("stream metadata") != std::string::npos ||
        lower.find("references") != std::string::npos ||
        lower.find("响应") != std::string::npos ||
        lower.find("解析") != std::string::npos)
    {
        return "serving/http";
    }
    if (lower.find("code_analysis") != std::string::npos ||
        lower.find("agent") != std::string::npos ||
        lower.find("search_code") != std::string::npos ||
        lower.find("read_file") != std::string::npos ||
        lower.find("list_files") != std::string::npos)
    {
        return "serving/core/agent";
    }
    if (lower.find("servingcontext") != std::string::npos ||
        lower.find("sessionexecutor") != std::string::npos ||
        lower.find("sessionmanager") != std::string::npos)
    {
        return "serving/core";
    }
    if (lower.find("rag") != std::string::npos || lower.find("hybrid") != std::string::npos || lower.find("retrieval") != std::string::npos)
        return "serving/rag";
    return "serving";
}
