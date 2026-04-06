#include "serving/core/agent/AgentExecutor.h"

#include "serving/core/EngineExecutor.h"
#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
#include "serving/core/agent/AgentParser.h"
#include "serving/core/agent/AgentPrompt.h"
#include "serving/core/agent/BuiltinTools.h"
#include "serving/core/agent/code_analysis/CodeAnalysisEvidence.h"
#include "serving/core/agent/code_analysis/CodeAnalysisFormatter.h"
#include "serving/core/agent/code_analysis/CodeAnalysisHeuristics.h"
#include "serving/core/agent/code_analysis/CodeAnalysisPlanner.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include <glog/logging.h>

namespace
{
constexpr const char *kCodeAnalysisMode = "code_analysis";
constexpr const char *kWebResearchMode = "web_research";
constexpr int kWebResearchDefaultMinSteps = 8;
constexpr int kAgentStepDefaultMaxTokens = 80;
constexpr int kAgentStepMinMaxTokens = 32;
constexpr int kAgentStepMaxTokensCap = 128;
constexpr size_t kAgentToolResultForModelMaxChars = 900;
constexpr size_t kAgentAssistantStateMaxChars = 700;
constexpr size_t kAgentModelInputMaxMessages = 14;
constexpr size_t kAgentModelInputMaxChars = 6000;
constexpr int kWebResearchDecompositionMaxTokens = 120;
constexpr int kWebResearchSynthesisMaxTokens = 180;

std::string normalize_agent_mode(const std::string &mode)
{
    if (mode.empty() || mode == "assistant" || mode == "code" || mode == "code_agent")
        return kCodeAnalysisMode;
    if (mode == "web" || mode == "research")
        return kWebResearchMode;
    if (mode != kCodeAnalysisMode && mode != kWebResearchMode)
        return kCodeAnalysisMode;
    return mode;
}

std::vector<std::string> default_tools_for_mode(const std::string &mode)
{
    if (mode == kCodeAnalysisMode)
    {
        return {
            "search_kb",
            "open_chunk",
            "search_code",
            "read_file",
            "list_files",
            "search_docs",
            "get_config",
            "get_server_status"};
    }

    if (mode == kWebResearchMode)
    {
        return {
            "search_kb",
            "open_chunk",
            "search_code",
            "read_file",
            "list_files",
            "search_docs",
            "search_web",
            "fetch_url",
            "get_config",
            "get_server_status"};
    }

    return {
        "search_docs",
        "get_config",
        "get_server_status"};
}

std::vector<std::string> sanitize_tools(const ToolRegistry &registry,
                                        const std::vector<std::string> &tools)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto &name : tools)
    {
        if (!registry.Has(name))
            continue;
        if (!seen.insert(name).second)
            continue;
        out.push_back(name);
    }
    return out;
}

bool is_tool_allowed(const std::vector<std::string> &allowed_tools, const std::string &name)
{
    return std::find(allowed_tools.begin(), allowed_tools.end(), name) != allowed_tools.end();
}

std::string truncate_text(const std::string &text, size_t max_chars)
{
    if (text.size() <= max_chars)
        return text;
    if (max_chars <= 32)
        return text.substr(0, max_chars);
    return text.substr(0, max_chars) + "\n...[truncated]";
}

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

std::string extract_last_user_query(const std::vector<Message> &messages)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        if (it->role != "user")
            continue;
        std::string q = trim_copy(it->content);
        if (!q.empty())
            return q;
    }
    return {};
}

size_t estimate_messages_chars(const std::vector<Message> &messages)
{
    size_t total = 0;
    for (const auto &m : messages)
        total += m.role.size() + m.content.size();
    return total;
}

void trim_agent_messages(std::vector<Message> &messages,
                         size_t keep_prefix_count,
                         size_t max_messages,
                         size_t max_chars)
{
    if (messages.empty())
        return;

    keep_prefix_count = std::min(keep_prefix_count, messages.size());
    const size_t hard_max_messages = std::max(max_messages, keep_prefix_count);
    const size_t hard_max_chars = std::max(max_chars, estimate_messages_chars(std::vector<Message>(messages.begin(), messages.begin() + keep_prefix_count)));

    auto erase_one_dynamic = [&]() -> bool
    {
        if (messages.size() <= keep_prefix_count)
            return false;
        messages.erase(messages.begin() + static_cast<std::ptrdiff_t>(keep_prefix_count));
        return true;
    };

    while (messages.size() > hard_max_messages)
    {
        if (!erase_one_dynamic())
            break;
    }

    while (estimate_messages_chars(messages) > hard_max_chars)
    {
        if (!erase_one_dynamic())
            break;
    }
}

void append_code_context(const ToolRegistry &registry,
                         const std::string &tool_name,
                         std::string &tool_output)
{
    if (tool_name != "search_code")
        return;

    const auto matches = ParseSearchCodeMatches(tool_output);
    if (matches.empty())
        return;

    const auto &match = matches.front();
    nlohmann::json read_input = {
        {"path", match.path},
        {"start_line", std::max(1, match.line - 20)},
        {"end_line", match.line + 20}};
    const std::string read_output = registry.Execute("read_file", read_input);
    if (read_output.empty())
        return;

    tool_output += "\n\nAUTO_READ_FILE_CONTEXT\n";
    tool_output += read_output;
}

std::string reference_source_for_evidence(const CodeEvidence &evidence)
{
    if (!evidence.reference_source.empty())
        return evidence.reference_source;
    if (evidence.source_type == "web" || evidence.source_type == "web_search")
        return "web";
    if (evidence.source_type == "docs" || evidence.source_type == "config")
        return "docs";
    return "repo_code";
}

nlohmann::json BuildAgentReferencesJson(const std::vector<CodeEvidence> &evidence)
{
    nlohmann::json refs = nlohmann::json::array();
    std::set<std::string> seen;
    for (const auto &item : evidence)
    {
        const std::string source = reference_source_for_evidence(item);
        const std::string primary = !item.url.empty() ? item.url : (!item.chunk_id.empty() ? item.chunk_id : item.path);
        if (primary.empty())
            continue;

        const std::string dedupe_key = source + "|" + primary + "|" +
                                       std::to_string(item.start_line) + "|" +
                                       std::to_string(item.end_line);
        if (!seen.insert(dedupe_key).second)
            continue;

        nlohmann::json ref = {
            {"source", source},
            {"source_type", item.source_type},
            {"snippet", TrimCodeAnalysisText(item.snippet, 180)},
        };
        if (!item.chunk_id.empty())
            ref["chunk_id"] = item.chunk_id;
        if (!item.path.empty() && source != "web")
            ref["path"] = item.path;
        if (!item.title.empty())
            ref["title"] = item.title;
        if (!item.url.empty())
            ref["url"] = item.url;
        if (!item.symbol.empty() && source != "web")
            ref["symbol"] = item.symbol;
        if (item.start_line > 0)
            ref["start_line"] = item.start_line;
        if (item.end_line > 0)
            ref["end_line"] = item.end_line;
        refs.push_back(std::move(ref));
    }
    return refs;
}

struct WebResearchSubquery
{
    std::string label;
    std::string source_kind;
    std::string query;
};

struct WebResearchSynthesisResult
{
    CodeAnalysisSynthesis synthesis;
    std::vector<std::string> references;
};

std::string normalize_model_json(std::string raw)
{
    const std::string think_start = "<think>";
    const std::string think_end = "</think>";
    while (true)
    {
        const auto start = raw.find(think_start);
        if (start == std::string::npos)
            break;

        const auto end = raw.find(think_end, start + think_start.size());
        if (end == std::string::npos)
        {
            raw.erase(start);
            break;
        }

        raw.erase(start, end + think_end.size() - start);
    }

    raw = trim_copy(std::move(raw));

    const std::string fence = "```";
    if (raw.rfind(fence, 0) == 0)
    {
        const auto first_nl = raw.find('\n');
        const auto last_fence = raw.rfind(fence);
        if (first_nl != std::string::npos && last_fence != std::string::npos && last_fence > first_nl)
            raw = raw.substr(first_nl + 1, last_fence - first_nl - 1);
    }

    const auto first = raw.find('{');
    const auto last = raw.rfind('}');
    if (first != std::string::npos && last != std::string::npos && first < last)
        raw = raw.substr(first, last - first + 1);

    return trim_copy(std::move(raw));
}

bool parse_model_json_object(const std::string &raw_output, nlohmann::json &parsed)
{
    const std::string normalized = normalize_model_json(raw_output);
    if (normalized.empty())
        return false;
    try
    {
        parsed = nlohmann::json::parse(normalized);
        return parsed.is_object();
    }
    catch (...)
    {
        return false;
    }
}

std::vector<std::string> json_string_array(const nlohmann::json &value, size_t limit)
{
    std::vector<std::string> out;
    if (value.is_string())
    {
        const std::string text = trim_copy(value.get<std::string>());
        if (!text.empty())
            out.push_back(text);
        return out;
    }
    if (!value.is_array())
        return out;

    for (const auto &item : value)
    {
        if (!item.is_string())
            continue;
        const std::string text = trim_copy(item.get<std::string>());
        if (text.empty())
            continue;
        out.push_back(text);
        if (out.size() >= limit)
            break;
    }
    return out;
}

bool has_any_tool(const std::vector<std::string> &allowed_tools, const std::vector<std::string> &names)
{
    for (const auto &name : names)
    {
        if (is_tool_allowed(allowed_tools, name))
            return true;
    }
    return false;
}

std::vector<std::string> filter_allowed_tools(const std::vector<std::string> &allowed_tools,
                                              const std::vector<std::string> &names)
{
    std::vector<std::string> out;
    for (const auto &name : names)
    {
        if (is_tool_allowed(allowed_tools, name))
            out.push_back(name);
    }
    return out;
}

std::string normalize_subquery_source(std::string source)
{
    source = to_lower_copy(trim_copy(std::move(source)));
    if (source == "local" || source == "repo" || source == "repo_code" || source == "docs" || source == "code")
        return "local";
    if (source == "web" || source == "external" || source == "internet")
        return "web";
    if (source == "hybrid" || source == "both" || source == "mixed")
        return "hybrid";
    return {};
}

std::string extract_first_http_url(std::string text)
{
    static const std::regex kUrlRe(R"((https?://[^\s<>"']+))",
                                   std::regex::icase | std::regex::optimize);
    std::smatch match;
    if (!std::regex_search(text, match, kUrlRe) || match.size() < 2)
        return {};

    text = trim_copy(match[1].str());
    while (!text.empty())
    {
        const char tail = text.back();
        if (tail == '.' || tail == ',' || tail == ';' || tail == ')' || tail == ']' || tail == '}' || tail == '"' || tail == '\'')
            text.pop_back();
        else
            break;
    }
    return text;
}

std::vector<WebResearchSubquery> BuildWebResearchSubqueries(const std::string &question,
                                                            const std::vector<std::string> &allowed_tools)
{
    const CodeAnalysisQuestionHints hints = ExtractCodeAnalysisHints(question);
    const CodeAnalysisQuestionType question_type = ClassifyCodeAnalysisQuestion(question);
    const std::string raw_question = TrimCodeAnalysisText(question, 160);
    const std::string code_query = BuildPrimarySearchQuery(question, hints);
    const std::string repo_query = code_query.empty() ? raw_question : code_query;
    const std::string direct_url = extract_first_http_url(question);
    const std::string lower = to_lower_copy(trim_copy(question));
    const bool has_latest_signal =
        lower.find("最新") != std::string::npos ||
        lower.find("recent") != std::string::npos ||
        lower.find("latest") != std::string::npos ||
        lower.find("today") != std::string::npos ||
        lower.find("news") != std::string::npos;

    const bool has_local_tools = has_any_tool(allowed_tools, {"search_kb", "open_chunk", "search_code", "read_file", "search_docs", "list_files", "get_config", "get_server_status"});
    const bool has_web_tools = is_tool_allowed(allowed_tools, "search_web");

    std::vector<WebResearchSubquery> out;
    auto add = [&](std::string label, std::string source_kind, std::string query)
    {
        source_kind = normalize_subquery_source(std::move(source_kind));
        query = trim_copy(std::move(query));
        if (query.empty() || source_kind.empty())
            return;
        if (source_kind == "local" && !has_local_tools)
            source_kind = has_web_tools ? "web" : "";
        else if (source_kind == "web" && !has_web_tools)
            source_kind = has_local_tools ? "local" : "";
        else if (source_kind == "hybrid")
        {
            if (!has_local_tools && has_web_tools)
                source_kind = "web";
            else if (!has_web_tools && has_local_tools)
                source_kind = "local";
            else if (!has_local_tools && !has_web_tools)
                source_kind.clear();
        }
        if (source_kind.empty())
            return;
        for (const auto &item : out)
        {
            if (item.source_kind == source_kind && item.query == query)
                return;
        }
        out.push_back({std::move(label), std::move(source_kind), std::move(query)});
    };

    add("local_repo", "local", repo_query);
    if (!code_query.empty() && code_query != repo_query)
        add("local_code", "local", code_query);
    if (question_type == CodeAnalysisQuestionType::config_interface || question_type == CodeAnalysisQuestionType::troubleshooting)
        add("local_docs", "local", raw_question + " docs");
    if (!direct_url.empty())
        add("web_direct_url", "web", direct_url);
    add(has_latest_signal ? "cross_check_latest" : "cross_check", has_web_tools && has_local_tools ? "hybrid" : (has_local_tools ? "local" : "web"), raw_question);
    add("web_background", "web", raw_question);
    if (has_latest_signal)
        add("web_latest", "web", raw_question + " latest");

    if (out.size() > 5)
        out.resize(5);
    return out;
}

std::string BuildWebResearchDecompositionUserMessage(const std::string &question,
                                                     CodeAnalysisQuestionType question_type,
                                                     const CodeAnalysisQuestionHints &hints)
{
    std::ostringstream oss;
    oss << "QUESTION: " << question << "\n";
    oss << "LIGHT_CLASSIFICATION: " << ToString(question_type) << "\n";
    if (!hints.primary_symbol.empty())
        oss << "PRIMARY_SYMBOL: " << hints.primary_symbol << "\n";
    if (!hints.primary_file_path.empty())
        oss << "PRIMARY_FILE: " << hints.primary_file_path << "\n";
    if (!hints.keywords.empty())
    {
        oss << "KEYWORDS:";
        for (size_t i = 0; i < hints.keywords.size() && i < 8; ++i)
            oss << (i == 0 ? " " : ", ") << hints.keywords[i];
        oss << "\n";
    }
    oss << "Return 3 to 5 subqueries.";
    return oss.str();
}

std::string BuildWebResearchSynthesisUserMessage(const std::string &question,
                                                 const std::vector<CodeEvidence> &evidence,
                                                 const std::vector<AgentTraceStep> &trace)
{
    std::ostringstream oss;
    oss << "QUESTION: " << question << "\n";
    oss << "EVIDENCE:\n";
    for (size_t i = 0; i < evidence.size(); ++i)
    {
        const auto &item = evidence[i];
        oss << "[E" << (i + 1) << "] source=" << reference_source_for_evidence(item)
            << " type=" << item.source_type;
        if (!item.path.empty() && item.path != item.url)
            oss << " path=" << item.path;
        if (!item.title.empty())
            oss << " title=" << item.title;
        if (!item.url.empty())
            oss << " url=" << item.url;
        if (!item.symbol.empty())
            oss << " symbol=" << item.symbol;
        if (item.start_line > 0)
            oss << " lines=" << item.start_line << "-" << item.end_line;
        oss << "\n";
        if (!item.why_relevant.empty())
            oss << "why=" << TrimCodeAnalysisText(item.why_relevant, 180) << "\n";
        if (!item.snippet.empty())
            oss << "snippet=" << TrimCodeAnalysisText(item.snippet, 220) << "\n";
    }

    if (!trace.empty())
    {
        oss << "TRACE:\n";
        for (size_t i = 0; i < trace.size() && i < 8; ++i)
        {
            oss << "- step=" << trace[i].step_index
                << " tool=" << trace[i].selected_tool
                << " reason=" << TrimCodeAnalysisText(trace[i].planner_reason, 120) << "\n";
        }
    }
    return oss.str();
}

std::vector<WebResearchSubquery> ParseWebResearchSubqueries(const nlohmann::json &parsed,
                                                            const std::string &question,
                                                            const std::vector<std::string> &allowed_tools)
{
    std::vector<WebResearchSubquery> out;
    const bool has_local_tools = has_any_tool(allowed_tools, {"search_kb", "open_chunk", "search_code", "read_file", "search_docs", "list_files", "get_config", "get_server_status"});
    const bool has_web_tools = is_tool_allowed(allowed_tools, "search_web");

    auto add = [&](std::string label, std::string source_kind, std::string query)
    {
        source_kind = normalize_subquery_source(std::move(source_kind));
        query = trim_copy(std::move(query));
        label = trim_copy(std::move(label));
        if (query.empty() || source_kind.empty())
            return;
        if (source_kind == "local" && !has_local_tools)
            source_kind = has_web_tools ? "web" : "";
        else if (source_kind == "web" && !has_web_tools)
            source_kind = has_local_tools ? "local" : "";
        else if (source_kind == "hybrid")
        {
            if (!has_local_tools && has_web_tools)
                source_kind = "web";
            else if (!has_web_tools && has_local_tools)
                source_kind = "local";
            else if (!has_local_tools && !has_web_tools)
                source_kind.clear();
        }
        if (source_kind.empty())
            return;
        if (label.empty())
            label = "subquery_" + std::to_string(out.size() + 1);
        for (const auto &item : out)
        {
            if (item.source_kind == source_kind && item.query == query)
                return;
        }
        out.push_back({std::move(label), std::move(source_kind), std::move(query)});
    };

    if (parsed.is_object() && parsed.contains("subqueries") && parsed["subqueries"].is_array())
    {
        for (const auto &item : parsed["subqueries"])
        {
            if (!item.is_object())
                continue;
            add(item.value("label", ""),
                item.value("source", item.value("source_kind", "")),
                item.value("query", item.value("search_query", "")));
            if (out.size() >= 5)
                break;
        }
    }

    if (out.size() < 3)
    {
        const auto fallback = BuildWebResearchSubqueries(question, allowed_tools);
        for (const auto &item : fallback)
        {
            add(item.label, item.source_kind, item.query);
            if (out.size() >= 5)
                break;
        }
    }

    return out;
}

WebResearchSynthesisResult ParseWebResearchSynthesis(const nlohmann::json &parsed)
{
    WebResearchSynthesisResult out;
    if (!parsed.is_object())
        return out;

    out.synthesis.summary = trim_copy(parsed.value("summary", parsed.value("answer", "")));
    if (parsed.contains("analysis"))
        out.synthesis.analysis = json_string_array(parsed["analysis"], 4);
    if (parsed.contains("risks"))
        out.synthesis.risks = json_string_array(parsed["risks"], 3);
    if (parsed.contains("next_steps"))
        out.synthesis.next_steps = json_string_array(parsed["next_steps"], 2);
    if (parsed.contains("references"))
        out.references = json_string_array(parsed["references"], 6);
    return out;
}

int reference_id_to_index(const std::string &reference_id, size_t evidence_size)
{
    if (evidence_size == 0)
        return -1;

    std::string text = to_lower_copy(trim_copy(reference_id));
    text.erase(std::remove(text.begin(), text.end(), '['), text.end());
    text.erase(std::remove(text.begin(), text.end(), ']'), text.end());
    if (!text.empty() && text.front() == 'e')
        text.erase(text.begin());
    if (text.empty())
        return -1;
    for (char ch : text)
    {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return -1;
    }

    const int idx = std::stoi(text) - 1;
    if (idx < 0 || static_cast<size_t>(idx) >= evidence_size)
        return -1;
    return idx;
}

std::vector<CodeEvidence> SelectReferencedEvidence(const std::vector<CodeEvidence> &evidence,
                                                   const std::vector<std::string> &references)
{
    std::vector<CodeEvidence> out;
    std::set<int> seen;
    for (const auto &reference : references)
    {
        const int idx = reference_id_to_index(reference, evidence.size());
        if (idx < 0 || !seen.insert(idx).second)
            continue;
        out.push_back(evidence[static_cast<size_t>(idx)]);
    }
    return out;
}

bool has_trace_for_input(const std::vector<AgentTraceStep> &trace,
                         const std::string &tool_name,
                         const nlohmann::json &tool_input)
{
    return std::any_of(trace.begin(), trace.end(), [&](const AgentTraceStep &step)
                       { return step.selected_tool == tool_name && step.tool_input == tool_input; });
}
} // namespace

AgentExecutor::AgentExecutor(EngineExecutor &executor, Options options)
    : executor_(executor), options_(std::move(options))
{
    BuiltinToolsOptions builtin_options;
    builtin_options.repo_root = options_.repo_root;
    builtin_options.docs_root = options_.docs_root;
    builtin_options.config_path = options_.config_path;
    builtin_options.max_tool_output_chars = options_.max_tool_output_chars;
    builtin_options.search_kb_handler = options_.search_kb_handler;
    builtin_options.open_chunk_handler = options_.open_chunk_handler;

    RegisterBuiltinTools(tool_registry_, builtin_options, [this]()
                         {
                             if (!status_provider_)
                                 return std::string("server status provider is unavailable.");
                             return status_provider_();
                         });
}

void AgentExecutor::SetStatusProvider(std::function<std::string()> provider)
{
    status_provider_ = std::move(provider);
}

void AgentExecutor::Run(const std::shared_ptr<ServingContext> &ctx)
{
    if (!ctx)
        return;

    if (!ctx->session)
    {
        ctx->error_message = "AgentExecutor: session is required";
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    if (ctx->cancelled.load(std::memory_order_acquire))
    {
        ctx->EmitFinish(FinishReason::cancelled);
        return;
    }

    ctx->agent_trace.clear();
    ctx->agent_evidence.clear();
    ctx->agent_structured_output = nlohmann::json::object();

    const std::string agent_mode = normalize_agent_mode(ctx->agent_mode);
    const int max_steps = ctx->agent_max_steps > 0
                              ? ctx->agent_max_steps
                              : (agent_mode == kWebResearchMode ? std::max(options_.default_max_steps, kWebResearchDefaultMinSteps) : options_.default_max_steps);
    std::vector<std::string> allowed_tools = ctx->agent_tools.empty()
                                                 ? default_tools_for_mode(agent_mode)
                                                 : ctx->agent_tools;
    allowed_tools = sanitize_tools(tool_registry_, allowed_tools);
    if (allowed_tools.empty())
        allowed_tools = sanitize_tools(tool_registry_, default_tools_for_mode(agent_mode));

    if (agent_mode == kCodeAnalysisMode)
    {
        RunCodeAnalysis(ctx, allowed_tools, max_steps);
        return;
    }

    if (agent_mode == kWebResearchMode)
    {
        RunWebResearch(ctx, allowed_tools, max_steps);
        return;
    }

    RunGenericAgent(ctx, allowed_tools, max_steps);
}

void AgentExecutor::RunCodeAnalysis(const std::shared_ptr<ServingContext> &ctx,
                                    const std::vector<std::string> &allowed_tools,
                                    int max_steps)
{
    const std::string question = extract_last_user_query(ctx->messages);
    CodeAnalysisPlanner planner(question, allowed_tools, max_steps);
    CodeAnalysisEvidenceStore evidence_store;

    auto emit_progress = [&](const std::string &text)
    {
        if (!ctx->stream || !ctx->on_chunk || text.empty() || ctx->finished.load(std::memory_order_acquire))
            return;
        StreamChunk chunk;
        chunk.delta = text;
        ctx->on_chunk(chunk);
    };

    for (int step = 0; step < max_steps; ++step)
    {
        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        const CodeAnalysisPlanStep next = planner.NextStep(evidence_store, ctx->agent_trace);
        if (next.tool_name.empty())
            break;

        emit_progress("[agent] step " + std::to_string(step + 1) + "/" + std::to_string(max_steps) +
                      ": tool " + next.tool_name + "\n");

        const std::string tool_output = tool_registry_.Execute(next.tool_name, next.tool_input);
        const size_t added = evidence_store.AddToolOutput(next.tool_name,
                                                          next.tool_input,
                                                          tool_output,
                                                          question,
                                                          planner.question_type());

        AgentTraceStep trace_step;
        trace_step.step_index = step + 1;
        trace_step.selected_tool = next.tool_name;
        trace_step.tool_input = next.tool_input;
        trace_step.tool_output_summary = truncate_text(tool_output, 1200);
        trace_step.evidence_count = evidence_store.Size();
        trace_step.planner_reason = next.planner_reason;
        ctx->agent_trace.push_back(trace_step);

        LOG(INFO) << "[agent][code_analysis] req=" << ctx->request_id
                  << " step=" << trace_step.step_index
                  << " tool=" << trace_step.selected_tool
                  << " added_evidence=" << added
                  << " total_evidence=" << trace_step.evidence_count
                  << " reason=" << trace_step.planner_reason;
    }

    const auto top_evidence = evidence_store.Top(6);
    const CodeAnalysisFinalAnswer answer = CodeAnalysisFormatter::Build(question,
                                                                        planner.question_type(),
                                                                        top_evidence,
                                                                        ctx->agent_trace);
    ctx->agent_evidence = answer.evidence;
    ctx->agent_structured_output = ToJson(answer);
    ctx->agent_structured_output["references"] = BuildAgentReferencesJson(answer.evidence);
    if (ctx->stream && ctx->agent_structured_output["references"].is_array() && !ctx->agent_structured_output["references"].empty())
        ctx->stream_metadata_json = nlohmann::json{{"references", ctx->agent_structured_output["references"]}}.dump();

    const std::string output = ctx->agent_output_format == "structured"
                                   ? CodeAnalysisFormatter::ToStructuredText(answer)
                                   : CodeAnalysisFormatter::ToText(answer);
    ctx->EmitDelta(output);
    ctx->EmitFinish(FinishReason::stop);
}

void AgentExecutor::RunWebResearch(const std::shared_ptr<ServingContext> &ctx,
                                   const std::vector<std::string> &allowed_tools,
                                   int max_steps)
{
    const std::string question = extract_last_user_query(ctx->messages);
    const std::string direct_url = extract_first_http_url(question);
    const CodeAnalysisQuestionType question_type = ClassifyCodeAnalysisQuestion(question);
    const CodeAnalysisQuestionHints hints = ExtractCodeAnalysisHints(question);
    CodeAnalysisEvidenceStore evidence_store;

    auto emit_progress = [&](const std::string &text)
    {
        if (!ctx->stream || !ctx->on_chunk || text.empty() || ctx->finished.load(std::memory_order_acquire))
            return;
        StreamChunk chunk;
        chunk.delta = text;
        ctx->on_chunk(chunk);
    };

    int model_step_index = 0;
    auto run_model_json = [&](const std::string &system_prompt,
                              const std::string &user_prompt,
                              int max_tokens,
                              const std::string &progress_text,
                              nlohmann::json &parsed,
                              std::string *raw_output = nullptr) -> bool
    {
        if (ctx->cancelled.load(std::memory_order_acquire))
            return false;

        emit_progress(progress_text);

        auto step_ctx = std::make_shared<ServingContext>();
        step_ctx->request_id = ctx->request_id + "-web-model-" + std::to_string(++model_step_index);
        step_ctx->session_id = ctx->session_id + "#agent#" + ctx->request_id + "#web";
        step_ctx->model = ctx->model;
        step_ctx->inference_backend = ctx->inference_backend;
        step_ctx->is_chat = true;
        step_ctx->stream = false;
        step_ctx->session = ctx->session;
        step_ctx->messages = {
            {"system", system_prompt},
            {"user", user_prompt}};
        step_ctx->params = ctx->params;
        step_ctx->params["max_tokens"] = std::to_string(std::max(kAgentStepMinMaxTokens, max_tokens));

        executor_.ExecuteAndWait(step_ctx);
        ctx->usage.prompt_tokens += step_ctx->usage.prompt_tokens;
        ctx->usage.completion_tokens += step_ctx->usage.completion_tokens;
        ctx->usage.total_tokens = ctx->usage.prompt_tokens + ctx->usage.completion_tokens;

        if (step_ctx->finish_reason == FinishReason::cancelled || ctx->cancelled.load(std::memory_order_acquire))
            return false;
        if (step_ctx->finish_reason == FinishReason::error || !step_ctx->error_message.empty())
        {
            LOG(WARNING) << "[agent][web_research] req=" << ctx->request_id
                         << " model step failed: "
                         << (step_ctx->error_message.empty() ? "unknown model error" : step_ctx->error_message);
            return false;
        }

        if (raw_output)
            *raw_output = step_ctx->final_text;
        if (!parse_model_json_object(step_ctx->final_text, parsed))
        {
            LOG(WARNING) << "[agent][web_research] req=" << ctx->request_id
                         << " model step returned non-json output";
            return false;
        }
        return true;
    };

    std::vector<WebResearchSubquery> subqueries = BuildWebResearchSubqueries(question, allowed_tools);
    nlohmann::json decomposition_json;
    if (direct_url.empty() &&
        run_model_json(BuildWebResearchDecompositionPrompt(),
                       BuildWebResearchDecompositionUserMessage(question, question_type, hints),
                       kWebResearchDecompositionMaxTokens,
                       "[agent] web_research planning\n",
                       decomposition_json))
    {
        const auto planned = ParseWebResearchSubqueries(decomposition_json, question, allowed_tools);
        if (!planned.empty())
            subqueries = planned;
    }

    nlohmann::json subqueries_json = nlohmann::json::array();
    for (const auto &item : subqueries)
    {
        subqueries_json.push_back({
            {"label", item.label},
            {"source", item.source_kind},
            {"query", item.query},
        });
    }

    int step_index = 0;
    auto run_tool = [&](const std::string &tool_name,
                        const nlohmann::json &tool_input,
                        const std::string &reason) -> std::string
    {
        if (step_index >= max_steps)
            return std::string();
        if (has_trace_for_input(ctx->agent_trace, tool_name, tool_input))
            return std::string();
        if (!is_tool_allowed(allowed_tools, tool_name))
            return std::string();

        ++step_index;
        emit_progress("[agent] step " + std::to_string(step_index) + "/" + std::to_string(max_steps) +
                      ": tool " + tool_name + "\n");

        const std::string tool_output = tool_registry_.Execute(tool_name, tool_input);
        evidence_store.AddToolOutput(tool_name, tool_input, tool_output, question, question_type);

        AgentTraceStep trace_step;
        trace_step.step_index = step_index;
        trace_step.selected_tool = tool_name;
        trace_step.tool_input = tool_input;
        trace_step.tool_output_summary = truncate_text(tool_output, 1200);
        trace_step.evidence_count = evidence_store.Size();
        trace_step.planner_reason = reason;
        ctx->agent_trace.push_back(trace_step);

        LOG(INFO) << "[agent][web_research] req=" << ctx->request_id
                  << " step=" << trace_step.step_index
                  << " tool=" << trace_step.selected_tool
                  << " total_evidence=" << trace_step.evidence_count
                  << " reason=" << trace_step.planner_reason;
        return tool_output;
    };

    auto run_local_subquery = [&](const WebResearchSubquery &subquery)
    {
        const auto local_tools = filter_allowed_tools(allowed_tools,
                                                      {"search_kb", "open_chunk", "search_code", "read_file", "list_files", "search_docs", "get_config", "get_server_status"});
        if (local_tools.empty() || step_index >= max_steps)
            return;

        const int planner_budget = std::min(2, max_steps - step_index);
        CodeAnalysisPlanner local_planner(subquery.query, local_tools, planner_budget);
        CodeAnalysisEvidenceStore local_store;
        std::vector<AgentTraceStep> local_trace;

        for (int i = 0; i < planner_budget; ++i)
        {
            const CodeAnalysisPlanStep next = local_planner.NextStep(local_store, local_trace);
            if (next.tool_name.empty())
                break;

            const std::string reason = "子查询 `" + subquery.label + "` [local] " + next.planner_reason;
            const std::string output = run_tool(next.tool_name, next.tool_input, reason);
            if (output.empty())
                break;

            local_store.AddToolOutput(next.tool_name, next.tool_input, output, question, question_type);

            AgentTraceStep trace_step;
            trace_step.step_index = static_cast<int>(local_trace.size()) + 1;
            trace_step.selected_tool = next.tool_name;
            trace_step.tool_input = next.tool_input;
            trace_step.tool_output_summary = truncate_text(output, 1200);
            trace_step.evidence_count = local_store.Size();
            trace_step.planner_reason = reason;
            local_trace.push_back(std::move(trace_step));
        }

        const bool should_probe_docs = local_store.Empty() ||
                                       question_type == CodeAnalysisQuestionType::config_interface ||
                                       question_type == CodeAnalysisQuestionType::troubleshooting;
        if (!should_probe_docs || step_index >= max_steps)
            return;

        if (is_tool_allowed(allowed_tools, "search_kb"))
        {
            const nlohmann::json input = {{"kb", "docs"}, {"query", subquery.query}, {"top_k", 2}, {"mode", "hybrid"}};
            const std::string output = run_tool("search_kb", input, "子查询 `" + subquery.label + "` [local] 补 docs KB 侧证据。");
            if (!output.empty())
            {
                local_store.AddToolOutput("search_kb", input, output, question, question_type);
                if (step_index < max_steps && is_tool_allowed(allowed_tools, "open_chunk"))
                {
                    const auto hits = ParseSearchKbHits(output);
                    if (!hits.empty())
                    {
                        const nlohmann::json open_input = {{"chunk_id", hits.front().chunk_id}};
                        const std::string open_output = run_tool("open_chunk", open_input, "docs KB 已命中 chunk，继续 open_chunk 精读。");
                        if (!open_output.empty())
                            local_store.AddToolOutput("open_chunk", open_input, open_output, question, question_type);
                    }
                }
            }
            return;
        }

        if (is_tool_allowed(allowed_tools, "search_docs"))
        {
            const nlohmann::json input = {{"query", subquery.query}};
            const std::string output = run_tool("search_docs", input, "子查询 `" + subquery.label + "` [local] 回退到 search_docs。");
            if (!output.empty())
                local_store.AddToolOutput("search_docs", input, output, question, question_type);
        }
    };

    auto run_web_subquery = [&](const WebResearchSubquery &subquery)
    {
        if (step_index >= max_steps || !is_tool_allowed(allowed_tools, "search_web"))
            return;

        const nlohmann::json input = {{"query", subquery.query}, {"top_k", 3}};
        const std::string output = run_tool("search_web", input, "子查询 `" + subquery.label + "` [web] 走外部网页搜索。");
        if (output.empty() || step_index >= max_steps || !is_tool_allowed(allowed_tools, "fetch_url"))
            return;

        const auto hits = ParseWebSearchHits(output);
        if (!hits.empty())
            run_tool("fetch_url", {{"url", hits.front().url}}, "网页搜索已命中结果，继续 fetch_url 抽取正文证据。");
    };

    for (const auto &subquery : subqueries)
    {
        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }
        if (step_index >= max_steps)
            break;

        if (subquery.source_kind == "local")
            run_local_subquery(subquery);
        else if (subquery.source_kind == "web")
            run_web_subquery(subquery);
        else if (subquery.source_kind == "hybrid")
        {
            run_local_subquery(subquery);
            if (step_index < max_steps)
                run_web_subquery(subquery);
        }
    }

    const auto top_evidence = evidence_store.Top(8);
    std::vector<CodeEvidence> selected_evidence = top_evidence;
    CodeAnalysisSynthesis synthesis;
    const CodeAnalysisSynthesis *synthesis_ptr = nullptr;

    if (!top_evidence.empty())
    {
        nlohmann::json synthesis_json;
        if (run_model_json(BuildWebResearchSynthesisPrompt(),
                           BuildWebResearchSynthesisUserMessage(question, top_evidence, ctx->agent_trace),
                           kWebResearchSynthesisMaxTokens,
                           "[agent] web_research synthesis\n",
                           synthesis_json))
        {
            const auto synthesized = ParseWebResearchSynthesis(synthesis_json);
            if (!synthesized.synthesis.summary.empty() ||
                !synthesized.synthesis.analysis.empty() ||
                !synthesized.synthesis.risks.empty() ||
                !synthesized.synthesis.next_steps.empty())
            {
                synthesis = synthesized.synthesis;
                synthesis_ptr = &synthesis;
            }
            const auto referenced = SelectReferencedEvidence(top_evidence, synthesized.references);
            if (!referenced.empty())
                selected_evidence = referenced;
        }
    }

    const CodeAnalysisFinalAnswer answer = CodeAnalysisFormatter::BuildWebResearch(question, selected_evidence, ctx->agent_trace, synthesis_ptr);
    ctx->agent_evidence = answer.evidence;
    ctx->agent_structured_output = ToJson(answer);
    ctx->agent_structured_output["mode"] = kWebResearchMode;
    ctx->agent_structured_output["subqueries"] = subqueries_json;
    ctx->agent_structured_output["references"] = BuildAgentReferencesJson(answer.evidence);
    if (ctx->stream && ctx->agent_structured_output["references"].is_array() && !ctx->agent_structured_output["references"].empty())
        ctx->stream_metadata_json = nlohmann::json{{"references", ctx->agent_structured_output["references"]}}.dump();

    const std::string output = ctx->agent_output_format == "structured"
                                   ? CodeAnalysisFormatter::ToStructuredText(answer)
                                   : CodeAnalysisFormatter::ToText(answer);
    ctx->EmitDelta(output);
    ctx->EmitFinish(FinishReason::stop);
}

void AgentExecutor::RunGenericAgent(const std::shared_ptr<ServingContext> &ctx,
                                    const std::vector<std::string> &allowed_tools,
                                    int max_steps)
{
    std::vector<Message> step_messages;
    step_messages.push_back({"system", BuildToolPrompt("assistant", allowed_tools)});
    step_messages.insert(step_messages.end(), ctx->messages.begin(), ctx->messages.end());
    const size_t fixed_prefix_count = step_messages.size();

    auto emit_progress = [&](const std::string &text)
    {
        if (!ctx->stream || !ctx->on_chunk || text.empty() || ctx->finished.load(std::memory_order_acquire))
            return;
        StreamChunk chunk;
        chunk.delta = text;
        ctx->on_chunk(chunk);
    };

    std::string last_model_output;
    for (int step = 0; step < max_steps; ++step)
    {
        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        auto step_ctx = std::make_shared<ServingContext>();
        step_ctx->request_id = ctx->request_id + "-agent-" + std::to_string(step + 1);
        step_ctx->session_id = ctx->session_id + "#agent#" + ctx->request_id;
        step_ctx->model = ctx->model;
        step_ctx->inference_backend = ctx->inference_backend;
        step_ctx->is_chat = true;
        step_ctx->stream = false;
        step_ctx->session = ctx->session;
        trim_agent_messages(step_messages,
                            fixed_prefix_count,
                            kAgentModelInputMaxMessages,
                            kAgentModelInputMaxChars);
        step_ctx->messages = step_messages;
        step_ctx->params = ctx->params;

        int step_max_tokens = kAgentStepDefaultMaxTokens;
        auto max_it = step_ctx->params.find("max_tokens");
        if (max_it != step_ctx->params.end())
        {
            try
            {
                step_max_tokens = std::stoi(max_it->second);
            }
            catch (...)
            {
                step_max_tokens = kAgentStepDefaultMaxTokens;
            }
        }
        step_max_tokens = std::clamp(step_max_tokens, kAgentStepMinMaxTokens, kAgentStepMaxTokensCap);
        step_ctx->params["max_tokens"] = std::to_string(step_max_tokens);
        emit_progress("[agent] step " + std::to_string(step + 1) + "/" + std::to_string(max_steps) +
                      ": model reasoning\n");

        executor_.ExecuteAndWait(step_ctx);

        ctx->usage.prompt_tokens += step_ctx->usage.prompt_tokens;
        ctx->usage.completion_tokens += step_ctx->usage.completion_tokens;
        ctx->usage.total_tokens = ctx->usage.prompt_tokens + ctx->usage.completion_tokens;

        if (step_ctx->finish_reason == FinishReason::cancelled)
        {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        if (step_ctx->finish_reason == FinishReason::error || !step_ctx->error_message.empty())
        {
            ctx->error_message = step_ctx->error_message.empty()
                                     ? "agent model step failed at step " + std::to_string(step + 1)
                                     : step_ctx->error_message;
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        last_model_output = step_ctx->final_text;
        const AgentAction action = ParseAgentAction(last_model_output);
        if (action.type == AgentAction::Type::final_answer)
        {
            const std::string answer = action.answer.empty() ? last_model_output : action.answer;
            if (!answer.empty())
                ctx->EmitDelta(answer);
            ctx->EmitFinish(FinishReason::stop);
            return;
        }

        if (action.tool_name.empty())
        {
            ctx->error_message = "AgentExecutor: tool call missing tool name";
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        std::string tool_output;
        if (!is_tool_allowed(allowed_tools, action.tool_name))
        {
            tool_output = "tool is not allowed for this request: " + action.tool_name;
        }
        else
        {
            tool_output = tool_registry_.Execute(action.tool_name, action.tool_input);
            append_code_context(tool_registry_, action.tool_name, tool_output);
        }

        LOG(INFO) << "[agent] req=" << ctx->request_id
                  << " step=" << (step + 1)
                  << " tool=" << action.tool_name;
        emit_progress("[agent] step " + std::to_string(step + 1) + "/" + std::to_string(max_steps) +
                      ": tool " + action.tool_name + "\n");

        step_messages.push_back({"assistant", truncate_text(last_model_output, kAgentAssistantStateMaxChars)});
        step_messages.push_back({"user",
                                 BuildToolResultMessage(action.tool_name,
                                                        truncate_text(tool_output, kAgentToolResultForModelMaxChars))});
        trim_agent_messages(step_messages,
                            fixed_prefix_count,
                            kAgentModelInputMaxMessages,
                            kAgentModelInputMaxChars);
    }

    ctx->error_message = "AgentExecutor: max_steps reached without final answer";
    if (!last_model_output.empty())
        ctx->error_message += ", last_output=" + truncate_text(last_model_output, 512);
    ctx->EmitFinish(FinishReason::error);
}
