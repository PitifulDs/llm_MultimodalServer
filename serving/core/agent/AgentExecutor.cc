#include "serving/core/agent/AgentExecutor.h"

#include "serving/core/EngineExecutor.h"
#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
#include "serving/core/agent/AgentParser.h"
#include "serving/core/agent/AgentPrompt.h"
#include "serving/core/agent/BuiltinTools.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <set>
#include <vector>

#include <glog/logging.h>

namespace
{
constexpr const char *kCodeAnalysisMode = "code_analysis";
constexpr int kAgentMinStepMaxTokens = 384;

std::string normalize_agent_mode(const std::string &mode)
{
    if (mode.empty() || mode == "assistant" || mode == "code" || mode == "code_agent")
        return kCodeAnalysisMode;
    if (mode != kCodeAnalysisMode)
        return kCodeAnalysisMode;
    return mode;
}

std::vector<std::string> default_tools_for_mode(const std::string &mode)
{
    if (mode == kCodeAnalysisMode)
    {
        return {
            "search_code",
            "read_file",
            "list_files",
            "search_docs",
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
    if (allowed_tools.empty())
        return true;

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

bool parse_first_search_code_match(const std::string &tool_output,
                                   std::string &path_out,
                                   int &line_out)
{
    std::istringstream iss(tool_output);
    std::string line;
    while (std::getline(iss, line))
    {
        const std::string prefix = "- file=";
        const auto pos = line.find(prefix);
        if (pos == std::string::npos)
            continue;

        const auto text_pos = line.find(" text=", pos + prefix.size());
        const std::string file_part = text_pos == std::string::npos
                                          ? line.substr(pos + prefix.size())
                                          : line.substr(pos + prefix.size(), text_pos - (pos + prefix.size()));

        const auto colon_pos = file_part.rfind(':');
        if (colon_pos == std::string::npos)
            continue;

        const std::string maybe_line = file_part.substr(colon_pos + 1);
        bool numeric = !maybe_line.empty();
        for (char ch : maybe_line)
        {
            if (!std::isdigit(static_cast<unsigned char>(ch)))
            {
                numeric = false;
                break;
            }
        }
        if (!numeric)
            continue;

        path_out = file_part.substr(0, colon_pos);
        line_out = std::stoi(maybe_line);
        return !path_out.empty();
    }
    return false;
}

std::vector<std::string> extract_evidence_lines(const std::string &tool_output,
                                                size_t limit = 4)
{
    std::vector<std::string> lines_out;
    std::istringstream iss(tool_output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.rfind("- file=", 0) == 0 || line.rfind("FILE ", 0) == 0)
        {
            lines_out.push_back(line);
            if (lines_out.size() >= limit)
                break;
        }
    }
    return lines_out;
}

bool answer_mentions_repo_file(const std::string &answer)
{
    static const char *tokens[] = {".cc", ".cpp", ".h", ".hpp", ".md", "CMakeLists", "/"};
    for (const auto *token : tokens)
    {
        if (answer.find(token) != std::string::npos)
            return true;
    }
    return false;
}

std::string append_evidence_if_needed(const std::string &answer,
                                      const std::vector<std::string> &evidence_lines,
                                      const std::vector<std::string> &tool_summaries)
{
    if (answer.empty() || answer_mentions_repo_file(answer))
        return answer;

    std::ostringstream oss;
    oss << answer;
    if (!evidence_lines.empty())
    {
        oss << "\n\nObserved code evidence:\n";
        for (const auto &line : evidence_lines)
            oss << line << "\n";
        return oss.str();
    }

    if (!tool_summaries.empty())
    {
        oss << "\n\nObserved tool result:\n";
        oss << tool_summaries.front() << "\n";
    }
    return oss.str();
}

void maybe_append_code_context(const ToolRegistry &registry,
                               const std::string &agent_mode,
                               const std::string &tool_name,
                               std::string &tool_output)
{
    if (agent_mode != kCodeAnalysisMode || tool_name != "search_code")
        return;

    std::string path;
    int line = 0;
    if (!parse_first_search_code_match(tool_output, path, line))
        return;

    const int start_line = std::max(1, line - 20);
    const int end_line = line + 20;
    nlohmann::json read_input = {
        {"path", path},
        {"start_line", start_line},
        {"end_line", end_line}};

    const std::string read_output = registry.Execute("read_file", read_input);
    if (read_output.empty())
        return;

    tool_output += "\n\nAUTO_READ_FILE_CONTEXT\n";
    tool_output += read_output;
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

    const int max_steps = ctx->agent_max_steps > 0 ? ctx->agent_max_steps : options_.default_max_steps;
    const std::string agent_mode = normalize_agent_mode(ctx->agent_mode);
    std::vector<std::string> allowed_tools = ctx->agent_tools.empty()
                                                 ? default_tools_for_mode(agent_mode)
                                                 : ctx->agent_tools;
    allowed_tools = sanitize_tools(tool_registry_, allowed_tools);
    if (allowed_tools.empty())
    {
        allowed_tools = sanitize_tools(tool_registry_, default_tools_for_mode(agent_mode));
    }

    auto shadow_session = std::make_shared<Session>(ctx->session_id + "#agent#" + ctx->request_id,
                                                    ctx->model,
                                                    ctx->inference_backend);
    {
        std::lock_guard<std::mutex> lk(ctx->session->mu);
        shadow_session->history = ctx->session->history;
    }

    std::vector<Message> step_messages;
    step_messages.push_back({"system", BuildToolPrompt(agent_mode, allowed_tools)});
    step_messages.insert(step_messages.end(), ctx->messages.begin(), ctx->messages.end());

    std::string last_model_output;
    std::vector<std::string> evidence_lines;
    std::vector<std::string> tool_summaries;
    bool has_observed_tool = false;

    for (int step = 0; step < max_steps; ++step)
    {
        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        auto step_ctx = std::make_shared<ServingContext>();
        step_ctx->request_id = ctx->request_id + "-agent-" + std::to_string(step + 1);
        step_ctx->session_id = shadow_session->session_id;
        step_ctx->model = ctx->model;
        step_ctx->inference_backend = ctx->inference_backend;
        step_ctx->is_chat = true;
        step_ctx->stream = false;
        step_ctx->session = shadow_session;
        step_ctx->messages = step_messages;
        step_ctx->params = ctx->params;

        auto max_it = step_ctx->params.find("max_tokens");
        if (max_it == step_ctx->params.end())
        {
            step_ctx->params["max_tokens"] = std::to_string(kAgentMinStepMaxTokens);
        }
        else
        {
            try
            {
                const int current = std::stoi(max_it->second);
                if (current < kAgentMinStepMaxTokens)
                    step_ctx->params["max_tokens"] = std::to_string(kAgentMinStepMaxTokens);
            }
            catch (...)
            {
                step_ctx->params["max_tokens"] = std::to_string(kAgentMinStepMaxTokens);
            }
        }

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
            ctx->error_message = step_ctx->error_message.empty() ? "agent model step failed" : step_ctx->error_message;
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        last_model_output = step_ctx->final_text;
        shadow_session->history.insert(shadow_session->history.end(), step_messages.begin(), step_messages.end());
        shadow_session->history.push_back({"assistant", last_model_output});
        shadow_session->touch();

        const AgentAction action = ParseAgentAction(last_model_output);
        if (action.type == AgentAction::Type::final_answer)
        {
            if (agent_mode == kCodeAnalysisMode && !has_observed_tool)
            {
                step_messages.clear();
                step_messages.push_back({"user",
                                         "For repository-specific or code-analysis questions, you must call a tool before giving the final answer. "
                                         "Call the most relevant tool now, usually search_code first and then read_file if needed. Return JSON only."});
                continue;
            }

            std::string answer = action.answer.empty() ? last_model_output : action.answer;
            if (agent_mode == kCodeAnalysisMode)
                answer = append_evidence_if_needed(answer, evidence_lines, tool_summaries);
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
            maybe_append_code_context(tool_registry_, agent_mode, action.tool_name, tool_output);
        }

        const auto new_evidence = extract_evidence_lines(tool_output);
        evidence_lines.insert(evidence_lines.end(), new_evidence.begin(), new_evidence.end());
        tool_summaries.push_back(truncate_text(tool_output, 600));
        has_observed_tool = true;

        LOG(INFO) << "[agent] req=" << ctx->request_id
                  << " step=" << (step + 1)
                  << " tool=" << action.tool_name;

        step_messages.clear();
        step_messages.push_back({"user", BuildToolResultMessage(action.tool_name, tool_output)});
    }

    ctx->error_message = "AgentExecutor: max_steps reached without final answer";
    if (!last_model_output.empty())
    {
        ctx->error_message += ", last_output=" + truncate_text(last_model_output, 512);
    }
    ctx->EmitFinish(FinishReason::error);
}
