#include "serving/core/agent/AgentExecutor.h"

#include "serving/core/EngineExecutor.h"
#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
#include "serving/core/agent/AgentParser.h"
#include "serving/core/agent/AgentPrompt.h"
#include "serving/core/agent/BuiltinTools.h"

#include <algorithm>
#include <set>
#include <vector>

#include <glog/logging.h>

namespace
{
constexpr const char *kCodeAnalysisMode = "code_analysis";

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

    auto shadow_session = std::make_shared<Session>(ctx->session_id + "#agent#" + ctx->request_id, ctx->model);
    {
        std::lock_guard<std::mutex> lk(ctx->session->mu);
        shadow_session->history = ctx->session->history;
    }

    std::vector<Message> step_messages;
    step_messages.push_back({"system", BuildToolPrompt(agent_mode, allowed_tools)});
    step_messages.insert(step_messages.end(), ctx->messages.begin(), ctx->messages.end());

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
        step_ctx->session_id = shadow_session->session_id;
        step_ctx->model = ctx->model;
        step_ctx->is_chat = true;
        step_ctx->stream = false;
        step_ctx->session = shadow_session;
        step_ctx->messages = step_messages;
        step_ctx->params = ctx->params;

        auto max_it = step_ctx->params.find("max_tokens");
        if (max_it == step_ctx->params.end())
        {
            step_ctx->params["max_tokens"] = "256";
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
        }

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
