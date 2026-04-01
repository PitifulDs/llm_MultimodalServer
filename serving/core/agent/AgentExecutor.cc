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
#include <set>
#include <sstream>
#include <vector>

#include <glog/logging.h>

namespace
{
constexpr const char *kCodeAnalysisMode = "code_analysis";
constexpr int kAgentStepDefaultMaxTokens = 80;
constexpr int kAgentStepMinMaxTokens = 32;
constexpr int kAgentStepMaxTokensCap = 128;
constexpr size_t kAgentToolResultForModelMaxChars = 900;
constexpr size_t kAgentAssistantStateMaxChars = 700;
constexpr size_t kAgentModelInputMaxMessages = 14;
constexpr size_t kAgentModelInputMaxChars = 6000;

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
            "search_kb",
            "open_chunk",
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

    const int max_steps = ctx->agent_max_steps > 0 ? ctx->agent_max_steps : options_.default_max_steps;
    const std::string agent_mode = normalize_agent_mode(ctx->agent_mode);
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
