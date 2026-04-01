#include "serving/core/agent/code_analysis/CodeAnalysisPlanner.h"

#include <algorithm>
#include <cctype>

#include "serving/core/agent/code_analysis/CodeAnalysisHeuristics.h"

namespace
{
std::string truncate_question(const std::string &question)
{
    return TrimCodeAnalysisText(question, 160);
}

std::string to_lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return s;
}
} // namespace

CodeAnalysisPlanner::CodeAnalysisPlanner(std::string question,
                                         std::vector<std::string> allowed_tools,
                                         int max_steps)
    : question_(std::move(question)),
      allowed_tools_(std::move(allowed_tools)),
      max_steps_(max_steps),
      question_type_(ClassifyCodeAnalysisQuestion(question_)),
      hints_(ExtractCodeAnalysisHints(question_))
{
}

const std::string &CodeAnalysisPlanner::question() const
{
    return question_;
}

CodeAnalysisQuestionType CodeAnalysisPlanner::question_type() const
{
    return question_type_;
}

const CodeAnalysisQuestionHints &CodeAnalysisPlanner::hints() const
{
    return hints_;
}

CodeAnalysisPlanStep CodeAnalysisPlanner::NextStep(const CodeAnalysisEvidenceStore &evidence_store,
                                                   const std::vector<AgentTraceStep> &trace) const
{
    if (max_steps_ > 0 && static_cast<int>(trace.size()) >= max_steps_)
        return {};

    if (trace.empty())
        return PickInitialStep();

    const auto &last = trace.back();
    if (last.selected_tool == "search_kb" && IsToolAllowed("open_chunk"))
    {
        const auto hits = ParseSearchKbHits(last.tool_output_summary);
        if (!hits.empty())
        {
            nlohmann::json input = {{"chunk_id", hits.front().chunk_id}};
            if (!HasToolInputTrace(trace, "open_chunk", input))
            {
                return {"open_chunk", input, "search_kb 已命中 chunk，继续 open_chunk 精读最相关证据。", true};
            }
        }
    }

    if (last.selected_tool == "search_code" && IsToolAllowed("read_file"))
    {
        const auto matches = ParseSearchCodeMatches(last.tool_output_summary);
        if (!matches.empty())
        {
            const auto &match = matches.front();
            nlohmann::json input = {
                {"path", match.path},
                {"start_line", std::max(1, match.line - 6)},
                {"end_line", match.line + 24}};
            if (!HasToolInputTrace(trace, "read_file", input))
            {
                return {"read_file", input, "search_code 已定位到候选代码，继续 read_file 拉取局部上下文。", true};
            }
        }
    }

    if (evidence_store.Size() >= 2 && evidence_store.HasFocusedContext())
        return {};

    return PickFallbackStep(evidence_store, trace);
}

bool CodeAnalysisPlanner::IsToolAllowed(const std::string &tool_name) const
{
    return std::find(allowed_tools_.begin(), allowed_tools_.end(), tool_name) != allowed_tools_.end();
}

bool CodeAnalysisPlanner::HasToolTrace(const std::vector<AgentTraceStep> &trace, const std::string &tool_name) const
{
    return std::any_of(trace.begin(), trace.end(), [&](const AgentTraceStep &step)
                       { return step.selected_tool == tool_name; });
}

bool CodeAnalysisPlanner::HasToolInputTrace(const std::vector<AgentTraceStep> &trace,
                                            const std::string &tool_name,
                                            const nlohmann::json &tool_input) const
{
    return std::any_of(trace.begin(), trace.end(), [&](const AgentTraceStep &step)
                       { return step.selected_tool == tool_name && step.tool_input == tool_input; });
}

CodeAnalysisPlanStep CodeAnalysisPlanner::PickInitialStep() const
{
    const std::string query = BuildPrimarySearchQuery(question_, hints_);
    const std::string preferred_path = InferPreferredSearchPath(question_, hints_);
    const std::string lower = to_lower_copy(question_);
    const bool config_like = lower.find("配置") != std::string::npos ||
                             lower.find("参数") != std::string::npos ||
                             lower.find("环境变量") != std::string::npos ||
                             lower.find("api") != std::string::npos ||
                             lower.find("接口") != std::string::npos;

    if ((!hints_.primary_symbol.empty() || !hints_.primary_file_path.empty()) && IsToolAllowed("search_code"))
    {
        nlohmann::json input = {{"query", query}, {"limit", 8}, {"path", preferred_path}};
        return {"search_code", input, "问题里出现了明确 symbol 或文件名，优先 search_code 做确定性定位。", true};
    }

    if (question_type_ == CodeAnalysisQuestionType::config_interface ||
        question_type_ == CodeAnalysisQuestionType::troubleshooting)
    {
        if (config_like && IsToolAllowed("search_docs"))
            return {"search_docs", {{"query", truncate_question(question_)}}, "配置、接口或排障问题先查 docs，避免一上来读整文件。", true};
        if (IsToolAllowed("search_code"))
            return {"search_code", {{"query", query}, {"limit", 8}, {"path", preferred_path}}, "这类问题更像代码定位，先在优先目录下 search_code。", true};
        if (IsToolAllowed("search_kb"))
            return {"search_kb", {{"kb", "repo_code"}, {"query", truncate_question(question_)}, {"top_k", 4}, {"mode", "hybrid"}}, "先从 repo_code 检索相关实现。", true};
        if (IsToolAllowed("get_config"))
            return {"get_config", nlohmann::json::object(), "没有 docs 工具时，先检查配置。", true};
    }

    if ((question_type_ == CodeAnalysisQuestionType::call_chain ||
         question_type_ == CodeAnalysisQuestionType::symbol_behavior) &&
        IsToolAllowed("search_code"))
    {
        nlohmann::json input = {{"query", query}, {"limit", 8}, {"path", preferred_path}};
        return {"search_code", input, "调用链和符号问题优先 search_code，先粗召回再精读。", true};
    }

    if (question_type_ == CodeAnalysisQuestionType::location_lookup && IsToolAllowed("search_code"))
        return {"search_code", {{"query", query}, {"limit", 8}, {"path", preferred_path}}, "定位类问题优先 search_code。", true};

    if (IsToolAllowed("search_kb"))
        return {"search_kb", {{"kb", "repo_code"}, {"query", truncate_question(question_)}, {"top_k", 4}, {"mode", "hybrid"}}, "仓库分析优先 search_kb，先拿 RAG 粗召回。", true};

    if (IsToolAllowed("search_code"))
        return {"search_code", {{"query", query}, {"limit", 8}, {"path", preferred_path}}, "缺少 search_kb 时退回 search_code。", true};

    if (IsToolAllowed("list_files"))
        return {"list_files", {{"path", hints_.primary_directory.empty() ? "serving" : hints_.primary_directory}, {"limit", 80}}, "没有检索工具时，至少先列目录定位模块。", true};

    return {};
}

CodeAnalysisPlanStep CodeAnalysisPlanner::PickFallbackStep(const CodeAnalysisEvidenceStore &evidence_store,
                                                           const std::vector<AgentTraceStep> &trace) const
{
    if (!HasToolTrace(trace, "search_kb") && IsToolAllowed("search_kb"))
    {
        return {"search_kb", {{"kb", "repo_code"}, {"query", truncate_question(question_)}, {"top_k", 4}, {"mode", "hybrid"}}, "当前证据不足，回退到 search_kb 扩大召回。", true};
    }

    if (!HasToolTrace(trace, "search_code") && IsToolAllowed("search_code"))
    {
        return {"search_code", {{"query", BuildPrimarySearchQuery(question_, hints_)}, {"limit", 8}, {"path", InferPreferredSearchPath(question_, hints_)}}, "当前证据不足，回退到 search_code 查符号和调用点。", true};
    }

    if (!HasToolTrace(trace, "search_docs") && IsToolAllowed("search_docs") &&
        (question_type_ == CodeAnalysisQuestionType::config_interface || question_type_ == CodeAnalysisQuestionType::troubleshooting))
    {
        return {"search_docs", {{"query", truncate_question(question_)}}, "继续查 docs 补接口或配置侧证据。", true};
    }

    if (!HasToolTrace(trace, "get_config") && IsToolAllowed("get_config") &&
        question_type_ == CodeAnalysisQuestionType::config_interface)
    {
        return {"get_config", nlohmann::json::object(), "补配置文件证据。", true};
    }

    if (!HasToolTrace(trace, "list_files") && IsToolAllowed("list_files") && evidence_store.Empty())
    {
        return {"list_files", {{"path", hints_.primary_directory.empty() ? "serving" : hints_.primary_directory}, {"limit", 80}}, "没有命中代码证据时，列目录做兜底定位。", true};
    }

    return {};
}
