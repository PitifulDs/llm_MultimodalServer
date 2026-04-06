#include "serving/core/agent/code_analysis/CodeAnalysisFormatter.h"

#include <algorithm>
#include <sstream>

#include "utils/json.hpp"
#include "serving/core/agent/code_analysis/CodeAnalysisHeuristics.h"

namespace
{
using json = nlohmann::json;

std::string to_lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return s;
}

std::string evidence_ref(const CodeEvidence &evidence)
{
    std::ostringstream oss;
    if (!evidence.url.empty() && (evidence.reference_source == "web" || evidence.path.empty() || evidence.path == evidence.url))
    {
        if (!evidence.title.empty())
            oss << evidence.title << " ";
        oss << evidence.url;
        return oss.str();
    }

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

bool try_parse_json_object(const std::string &raw, json &out)
{
    out = json::parse(raw, nullptr, false);
    return !out.is_discarded() && out.is_object();
}

std::string json_scalar_to_string(const json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float())
    {
        std::ostringstream oss;
        oss << value.get<double>();
        return oss.str();
    }
    if (value.is_null())
        return "null";
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool load_config_json(const std::vector<CodeEvidence> &evidence, json &cfg)
{
    for (const auto &item : evidence)
    {
        if (item.source_type != "config" && item.path != "config.json" && item.path != "runtime")
            continue;
        if (try_parse_json_object(item.snippet, cfg))
            return true;
    }
    return false;
}

bool has_any_evidence_text(const std::vector<CodeEvidence> &evidence, const std::vector<std::string> &terms)
{
    for (const auto &item : evidence)
    {
        const std::string path = to_lower_copy(item.path);
        const std::string symbol = to_lower_copy(item.symbol);
        const std::string snippet = to_lower_copy(item.snippet);
        for (const auto &term : terms)
        {
            if ((!path.empty() && path.find(term) != std::string::npos) ||
                (!symbol.empty() && symbol.find(term) != std::string::npos) ||
                (!snippet.empty() && snippet.find(term) != std::string::npos))
            {
                return true;
            }
        }
    }
    return false;
}

std::string build_config_summary(const std::string &question,
                                 const std::vector<CodeEvidence> &evidence)
{
    const std::string lower_question = to_lower_copy(question);
    json cfg;
    const bool has_cfg = load_config_json(evidence, cfg);
    std::vector<std::string> parts;

    if (lower_question.find("默认模型") != std::string::npos ||
        lower_question.find("default model") != std::string::npos ||
        lower_question.find("model") != std::string::npos)
    {
        if (has_cfg && cfg.contains("default_model"))
            parts.push_back("默认模型是 `" + json_scalar_to_string(cfg["default_model"]) + "`");
    }

    if (lower_question.find("max_tokens") != std::string::npos ||
        lower_question.find("max token") != std::string::npos)
    {
        if (has_cfg && cfg.contains("default_max_tokens"))
            parts.push_back("服务默认 `max_tokens` 是 `" + json_scalar_to_string(cfg["default_max_tokens"]) + "`");
    }

    if (lower_question.find("后端") != std::string::npos ||
        lower_question.find("backend") != std::string::npos)
    {
        std::string backend_fact;
        std::string backend_tail;
        if (has_cfg && cfg.contains("default_model") && cfg["default_model"].is_string() &&
            cfg.contains("models") && cfg["models"].is_object())
        {
            const std::string default_model = cfg["default_model"].get<std::string>();
            const auto model_it = cfg["models"].find(default_model);
            if (model_it != cfg["models"].end() && model_it->is_object() &&
                model_it->contains("backend"))
            {
                backend_fact = "默认模型 `" + default_model + "` 在配置里声明的后端是 `" +
                               json_scalar_to_string((*model_it)["backend"]) + "`";
            }
        }
        if (backend_fact.empty() && has_cfg && cfg.contains("serving_backend"))
            backend_fact = "服务默认后端配置是 `" + json_scalar_to_string(cfg["serving_backend"]) + "`";
        if (has_any_evidence_text(evidence, {"inference_backend", "get_inference_backend", "chatrequestparser"}))
            backend_tail = "；但具体某一次请求实际走 `local` 还是 `rpc`，要看请求里的 `inference_backend` 字段";
        else if (!backend_fact.empty())
            backend_tail = "；仅凭当前问题无法确认你这一次请求实际选的是 `local` 还是 `rpc`";
        if (!backend_fact.empty())
            parts.push_back(backend_fact + backend_tail);
    }

    if (parts.empty())
        return "";

    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
            oss << "；";
        oss << parts[i];
    }
    oss << "。";
    return oss.str();
}

CodeAnalysisFinalAnswer build_web_research_fallback(const std::string &question,
                                                    const std::vector<CodeEvidence> &evidence,
                                                    const std::vector<AgentTraceStep> &trace)
{
    CodeAnalysisFinalAnswer answer;
    answer.evidence = evidence;

    if (evidence.empty())
    {
        answer.summary = "当前还没有收集到足够的仓库或网页证据。";
        answer.analysis.push_back("`web_research` 已执行，但本地 KB 和外部网页都没有形成稳定证据。");
        answer.risks.push_back("缺少引用来源时，继续总结容易把猜测当成事实。");
        answer.next_steps.push_back("扩大检索词，分别补 repo/docs/web 侧证据。");
        return answer;
    }

    int repo_hits = 0;
    int docs_hits = 0;
    int web_hits = 0;
    for (const auto &item : evidence)
    {
        if (item.reference_source == "web")
            ++web_hits;
        else if (item.reference_source == "docs")
            ++docs_hits;
        else
            ++repo_hits;
    }

    answer.summary = "针对“" + TrimCodeAnalysisText(question, 56) + "”，已汇总本地仓库与外部网页证据，首个高相关来源是 " + evidence_ref(evidence.front()) + "。";

    if (!trace.empty())
    {
        std::ostringstream trace_line;
        trace_line << "工具链：";
        for (size_t i = 0; i < trace.size() && i < 5; ++i)
        {
            if (i > 0)
                trace_line << " -> ";
            trace_line << trace[i].selected_tool;
        }
        answer.analysis.push_back(trace_line.str());
    }

    answer.analysis.push_back("证据分布：repo_code=" + std::to_string(repo_hits) +
                              " docs=" + std::to_string(docs_hits) +
                              " web=" + std::to_string(web_hits) + "。");

    for (size_t i = 0; i < evidence.size() && i < 4; ++i)
    {
        const auto &item = evidence[i];
        std::string line = evidence_ref(item);
        if (!item.why_relevant.empty())
            line += " 表明：" + item.why_relevant;
        if (!item.snippet.empty())
            line += " 片段：" + TrimCodeAnalysisText(item.snippet, 120);
        answer.analysis.push_back(std::move(line));
    }

    if (repo_hits == 0)
        answer.risks.push_back("当前没有 repo_code 侧证据，仓库内实现细节仍可能缺失。");
    if (web_hits == 0)
        answer.risks.push_back("当前没有 web 侧证据，仓库外背景或最新信息未验证。");
    if (!trace.empty() && trace.back().selected_tool != "read_file" &&
        trace.back().selected_tool != "open_chunk" &&
        trace.back().selected_tool != "fetch_url")
    {
        answer.risks.push_back("最后一步不是精读型工具，局部上下文可能仍不完整。");
    }

    answer.next_steps.push_back("如需更强结论，可继续围绕最高相关 repo 证据做 read_file/open_chunk 深读。");
    if (web_hits > 0)
        answer.next_steps.push_back("如需更高时效性，可对首个 web 来源再次 fetch_url 或扩展更多站点。");

    return answer;
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

    if (question_type == CodeAnalysisQuestionType::config_interface)
    {
        const std::string config_summary = build_config_summary(question, evidence);
        if (!config_summary.empty())
            answer.summary = config_summary;
    }

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
    const bool has_direct_config_summary =
        question_type == CodeAnalysisQuestionType::config_interface &&
        answer.summary != fallback_summary(question_type);
    if (has_direct_config_summary)
        answer.next_steps.clear();
    else if (question_type == CodeAnalysisQuestionType::config_interface)
        answer.next_steps.push_back("如需确认配置生效链路，可继续查看 get_config 命中的字段与 HttpGateway/SessionExecutor 的读取位置。");
    if (!has_direct_config_summary && answer.next_steps.empty())
        answer.next_steps.push_back("若需要更完整结论，可继续围绕首个证据文件做 read_file 扩展上下文。");

    return answer;
}

CodeAnalysisFinalAnswer CodeAnalysisFormatter::BuildWebResearch(const std::string &question,
                                                               const std::vector<CodeEvidence> &evidence,
                                                               const std::vector<AgentTraceStep> &trace,
                                                               const CodeAnalysisSynthesis *synthesis)
{
    CodeAnalysisFinalAnswer answer = build_web_research_fallback(question, evidence, trace);
    if (synthesis)
    {
        if (!synthesis->summary.empty())
            answer.summary = synthesis->summary;
        if (!synthesis->analysis.empty())
            answer.analysis = synthesis->analysis;
        if (!synthesis->risks.empty())
            answer.risks = synthesis->risks;
        if (!synthesis->next_steps.empty())
            answer.next_steps = synthesis->next_steps;
        answer.evidence = evidence;
    }
    return answer;
}

std::string CodeAnalysisFormatter::ToText(const CodeAnalysisFinalAnswer &answer)
{
    std::ostringstream oss;
    if (!answer.summary.empty())
    {
        oss << "结论：\n";
        oss << "- " << answer.summary;
    }

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
