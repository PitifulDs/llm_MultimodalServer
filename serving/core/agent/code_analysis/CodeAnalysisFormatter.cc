#include "serving/core/agent/code_analysis/CodeAnalysisFormatter.h"

#include <algorithm>
#include <sstream>

#include "serving/core/agent/code_analysis/CodeAnalysisHeuristics.h"

namespace
{
std::string to_lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return s;
}

std::string evidence_ref(const CodeEvidence &evidence)
{
    std::ostringstream oss;
    oss << evidence.path;
    if (evidence.start_line > 0)
        oss << ":" << evidence.start_line;
    if (evidence.end_line > evidence.start_line)
        oss << "-" << evidence.end_line;
    if (!evidence.symbol.empty())
        oss << " (" << evidence.symbol << ")";
    return oss.str();
}

std::string fallback_summary(CodeAnalysisQuestionType question_type)
{
    switch (question_type)
    {
    case CodeAnalysisQuestionType::call_chain:
        return "当前证据不足以确认完整调用链。";
    case CodeAnalysisQuestionType::module_responsibility:
        return "当前证据不足以稳定判断模块职责。";
    case CodeAnalysisQuestionType::symbol_behavior:
        return "当前证据不足以稳定解释该函数或类的行为。";
    case CodeAnalysisQuestionType::config_interface:
        return "当前证据不足以确认接口或配置落点。";
    case CodeAnalysisQuestionType::location_lookup:
        return "当前证据不足以确认精确位置。";
    case CodeAnalysisQuestionType::troubleshooting:
        return "当前证据不足以确认排障结论。";
    case CodeAnalysisQuestionType::unknown:
    default:
        return "当前证据不足以给出强结论。";
    }
}
} // namespace

CodeAnalysisFinalAnswer CodeAnalysisFormatter::Build(const std::string &question,
                                                     CodeAnalysisQuestionType question_type,
                                                     const std::vector<CodeEvidence> &evidence,
                                                     const std::vector<AgentTraceStep> &trace)
{
    (void)question;
    CodeAnalysisFinalAnswer answer;
    answer.evidence = evidence;

    if (evidence.empty())
    {
        answer.summary = fallback_summary(question_type);
        answer.analysis.push_back("没有收集到足够的代码证据，当前只能给出保守结论。");
        answer.risks.push_back("若继续下强结论，容易把文档描述或模糊命中误判为真实实现。");
        answer.next_steps.push_back("扩大检索范围，优先 search_kb 或 search_code 后再 read_file/open_chunk。");
        return answer;
    }

    const auto &top = evidence.front();
    answer.summary = "针对“" + TrimCodeAnalysisText(question, 48) + "”，最相关实现位于 " + evidence_ref(top) + "。";

    if (!trace.empty())
    {
        std::ostringstream trace_line;
        trace_line << "工具链：";
        for (size_t i = 0; i < trace.size() && i < 3; ++i)
        {
            if (i > 0)
                trace_line << " -> ";
            trace_line << trace[i].selected_tool;
        }
        answer.analysis.push_back(trace_line.str());
    }

    const std::string lower_question = to_lower_copy(question);
    if (lower_question.find("默认工具链") != std::string::npos || lower_question.find("default tool") != std::string::npos)
    {
        answer.analysis.push_back("默认工具链包含 search_kb、open_chunk、search_code、read_file，以及 list_files / search_docs / get_config / get_server_status。");
    }

    for (size_t i = 0; i < evidence.size() && i < 3; ++i)
    {
        const auto &item = evidence[i];
        std::string line = evidence_ref(item);
        if (!item.why_relevant.empty())
            line += " 表明：" + item.why_relevant;
        if (!item.snippet.empty())
            line += " 片段：" + TrimCodeAnalysisText(item.snippet, 120);
        answer.analysis.push_back(std::move(line));
    }

    const bool enough = evidence.size() >= 2;
    if (!enough)
        answer.risks.push_back("目前只有单点证据，结论可能缺少跨文件或上下游调用验证。");
    if (!trace.empty() && trace.back().selected_tool != "read_file" && trace.back().selected_tool != "open_chunk")
        answer.risks.push_back("最后一步不是精读上下文，局部理解可能仍不完整。");

    if (question_type == CodeAnalysisQuestionType::call_chain && evidence.size() < 2)
        answer.next_steps.push_back("继续针对关键 symbol 做 search_code，并 read_file 查看上下游调用点。");
    if (question_type == CodeAnalysisQuestionType::config_interface)
        answer.next_steps.push_back("如需确认配置生效链路，可继续查看 get_config 命中的字段与 HttpGateway/SessionExecutor 的读取位置。");
    if (answer.next_steps.empty())
        answer.next_steps.push_back("若需要更完整结论，可继续围绕首个证据文件做 read_file 扩展上下文。");

    return answer;
}

std::string CodeAnalysisFormatter::ToText(const CodeAnalysisFinalAnswer &answer)
{
    std::ostringstream oss;
    oss << answer.summary;

    if (!answer.analysis.empty())
    {
        oss << "\n\n分析：\n";
        for (const auto &item : answer.analysis)
            oss << "- " << item << "\n";
    }

    if (!answer.evidence.empty())
    {
        oss << "\n证据：\n";
        for (size_t i = 0; i < answer.evidence.size() && i < 4; ++i)
            oss << "- " << evidence_ref(answer.evidence[i]) << "\n";
    }

    if (!answer.risks.empty())
    {
        oss << "\n风险：\n";
        for (const auto &item : answer.risks)
            oss << "- " << item << "\n";
    }

    if (!answer.next_steps.empty())
    {
        oss << "\n下一步：\n";
        for (const auto &item : answer.next_steps)
            oss << "- " << item << "\n";
    }

    return oss.str();
}

std::string CodeAnalysisFormatter::ToStructuredText(const CodeAnalysisFinalAnswer &answer)
{
    return ToJson(answer).dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}
