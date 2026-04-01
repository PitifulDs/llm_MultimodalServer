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

bool parse_first_search_kb_hit(const std::string &tool_output,
                               std::string &chunk_id_out,
                               std::string &path_out,
                               int &start_line_out,
                               int &end_line_out,
                               std::string &symbol_out)
{
    std::istringstream iss(tool_output);
    std::string line;
    while (std::getline(iss, line))
    {
        const std::string prefix = "- chunk_id=";
        if (line.rfind(prefix, 0) != 0)
            continue;

        const auto path_pos = line.find(" path=");
        if (path_pos == std::string::npos)
            continue;
        chunk_id_out = line.substr(prefix.size(), path_pos - prefix.size());

        const auto symbol_pos = line.find(" symbol=", path_pos + 6);
        if (symbol_pos == std::string::npos)
            continue;
        const std::string path_part = line.substr(path_pos + 6, symbol_pos - (path_pos + 6));
        const auto colon_pos = path_part.rfind(':');
        const auto dash_pos = path_part.rfind('-');
        if (colon_pos == std::string::npos || dash_pos == std::string::npos || dash_pos < colon_pos)
            continue;

        path_out = path_part.substr(0, colon_pos);
        try
        {
            start_line_out = std::stoi(path_part.substr(colon_pos + 1, dash_pos - colon_pos - 1));
            const auto score_pos = line.find(" score=", symbol_pos + 8);
            const size_t symbol_end = score_pos == std::string::npos ? std::string::npos : score_pos - (symbol_pos + 8);
            end_line_out = std::stoi(path_part.substr(dash_pos + 1));
            symbol_out = line.substr(symbol_pos + 8, symbol_end);
        }
        catch (...)
        {
            continue;
        }
        return !chunk_id_out.empty() && !path_out.empty();
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

size_t estimate_messages_chars(const std::vector<Message> &messages)
{
    size_t total = 0;
    for (const auto &m : messages)
    {
        total += m.role.size();
        total += m.content.size();
    }
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

std::string build_fast_rag_answer(const std::string &query,
                                  const std::string &path,
                                  int start_line,
                                  int end_line,
                                  const std::string &symbol,
                                  const std::string &chunk_text)
{
    std::string snippet = truncate_text(chunk_text, 240);
    std::replace(snippet.begin(), snippet.end(), '\n', ' ');

    std::ostringstream oss;
    oss << "我先检索了知识库。";
    if (!symbol.empty())
        oss << "最相关命中是 " << path << ":" << start_line << "-" << end_line << " 的 `" << symbol << "`。";
    else
        oss << "最相关命中是 " << path << ":" << start_line << "-" << end_line << "。";
    if (!query.empty())
        oss << "它与问题“" << truncate_text(query, 60) << "”直接相关。";
    if (!snippet.empty())
        oss << "片段摘要：" << snippet;
    return oss.str();
}

bool should_use_tool_only_fast_path(const std::vector<std::string> &allowed_tools)
{
    if (allowed_tools.empty())
        return false;
    for (const auto &tool : allowed_tools)
    {
        if (tool != "search_kb" && tool != "open_chunk")
            return false;
    }
    return std::find(allowed_tools.begin(), allowed_tools.end(), "search_kb") != allowed_tools.end();
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

    std::vector<Message> step_messages;
    step_messages.push_back({"system", BuildToolPrompt(agent_mode, allowed_tools)});
    step_messages.insert(step_messages.end(), ctx->messages.begin(), ctx->messages.end());
    const size_t fixed_prefix_count = step_messages.size();

    std::string last_model_output;
    std::vector<std::string> evidence_lines;
    std::vector<std::string> tool_summaries;
    bool has_observed_tool = false;
    const std::string fallback_search_query = extract_last_user_query(ctx->messages);
    auto emit_progress = [&](const std::string &text)
    {
        if (!ctx->stream || !ctx->on_chunk || text.empty())
            return;
        if (ctx->finished.load(std::memory_order_acquire))
            return;
        StreamChunk chunk;
        chunk.delta = text;
        chunk.is_finished = false;
        ctx->on_chunk(chunk);
    };

    if (agent_mode == kCodeAnalysisMode && should_use_tool_only_fast_path(allowed_tools))
    {
        const std::string auto_query = fallback_search_query.empty() ? std::string("EdgeLLM-Serving") : fallback_search_query;
        nlohmann::json search_input = {
            {"kb", "repo_code"},
            {"query", truncate_text(auto_query, 160)},
            {"top_k", 3},
            {"mode", "hybrid"}};

        emit_progress("[agent] fast-path: search_kb\n");
        const std::string search_output = tool_registry_.Execute("search_kb", search_input);
        std::string chunk_id;
        std::string path;
        std::string symbol;
        int start_line = 0;
        int end_line = 0;
        if (!parse_first_search_kb_hit(search_output, chunk_id, path, start_line, end_line, symbol))
        {
            ctx->error_message = "AgentExecutor: search_kb fast-path produced no usable hit";
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        std::string chunk_text;
        if (is_tool_allowed(allowed_tools, "open_chunk"))
        {
            emit_progress("[agent] fast-path: open_chunk\n");
            chunk_text = tool_registry_.Execute("open_chunk", {{"chunk_id", chunk_id}});
        }

        const std::string answer = build_fast_rag_answer(auto_query, path, start_line, end_line, symbol, chunk_text);
        LOG(INFO) << "[agent] req=" << ctx->request_id << " fast_path=search_kb_open_chunk";
        ctx->EmitDelta(answer);
        ctx->EmitFinish(FinishReason::stop);
        return;
    }

    for (int step = 0; step < max_steps; ++step)
    {
        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        // Bootstrap for code-analysis mode: run one deterministic search first to
        // avoid spending the first model turn on tool-selection chatter.
        if (step == 0 &&
            agent_mode == kCodeAnalysisMode &&
            !has_observed_tool &&
            is_tool_allowed(allowed_tools, "search_code"))
        {
            const std::string auto_query = fallback_search_query.empty() ? std::string("ThreadPool") : fallback_search_query;
            nlohmann::json auto_input = {
                {"query", truncate_text(auto_query, 160)},
                {"limit", 8},
                {"path", "serving"}};

            std::string tool_output = tool_registry_.Execute("search_code", auto_input);
            if (tool_output.find("- file=") == std::string::npos)
            {
                nlohmann::json fallback_input = {
                    {"query", truncate_text(auto_query, 160)},
                    {"limit", 8}};
                tool_output = tool_registry_.Execute("search_code", fallback_input);
            }
            maybe_append_code_context(tool_registry_, agent_mode, "search_code", tool_output);

            const auto new_evidence = extract_evidence_lines(tool_output);
            evidence_lines.insert(evidence_lines.end(), new_evidence.begin(), new_evidence.end());
            tool_summaries.push_back(truncate_text(tool_output, 600));
            has_observed_tool = true;

            LOG(INFO) << "[agent] req=" << ctx->request_id
                      << " step=" << (step + 1)
                      << " tool=search_code(bootstrap)";
            emit_progress("[agent] step " + std::to_string(step + 1) + "/" + std::to_string(max_steps) +
                          ": search_code (bootstrap)\n");

            step_messages.push_back({"user",
                                     BuildToolResultMessage("search_code",
                                                            truncate_text(tool_output, kAgentToolResultForModelMaxChars))});
            trim_agent_messages(step_messages,
                                fixed_prefix_count,
                                kAgentModelInputMaxMessages,
                                kAgentModelInputMaxChars);
            continue;
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
            if (step_ctx->error_message.empty())
            {
                ctx->error_message = "agent model step failed at step " + std::to_string(step + 1) +
                                     ", backend=" + (ctx->inference_backend.empty() ? "auto" : ctx->inference_backend);
            }
            else
            {
                ctx->error_message = step_ctx->error_message;
            }
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        last_model_output = step_ctx->final_text;

        const AgentAction action = ParseAgentAction(last_model_output);
        if (action.type == AgentAction::Type::final_answer)
        {
            if (agent_mode == kCodeAnalysisMode && !has_observed_tool)
            {
                if (is_tool_allowed(allowed_tools, "search_code"))
                {
                    const std::string auto_query = fallback_search_query.empty() ? std::string("ThreadPool") : fallback_search_query;
                    nlohmann::json auto_input = {
                        {"query", truncate_text(auto_query, 160)},
                        {"limit", 8},
                        {"path", "serving"}};

                    std::string tool_output = tool_registry_.Execute("search_code", auto_input);
                    if (tool_output.find("- file=") == std::string::npos)
                    {
                        nlohmann::json fallback_input = {
                            {"query", truncate_text(auto_query, 160)},
                            {"limit", 8}};
                        tool_output = tool_registry_.Execute("search_code", fallback_input);
                    }
                    maybe_append_code_context(tool_registry_, agent_mode, "search_code", tool_output);

                    const auto new_evidence = extract_evidence_lines(tool_output);
                    evidence_lines.insert(evidence_lines.end(), new_evidence.begin(), new_evidence.end());
                    tool_summaries.push_back(truncate_text(tool_output, 600));
                    has_observed_tool = true;

                    LOG(INFO) << "[agent] req=" << ctx->request_id
                              << " step=" << (step + 1)
                              << " tool=search_code(auto)";
                    emit_progress("[agent] step " + std::to_string(step + 1) + "/" + std::to_string(max_steps) +
                                  ": search_code (auto-recover)\n");

                    step_messages.push_back({"assistant", truncate_text(last_model_output, kAgentAssistantStateMaxChars)});
                    step_messages.push_back({"user",
                                             BuildToolResultMessage("search_code",
                                                                    truncate_text(tool_output, kAgentToolResultForModelMaxChars))});
                    trim_agent_messages(step_messages,
                                        fixed_prefix_count,
                                        kAgentModelInputMaxMessages,
                                        kAgentModelInputMaxChars);
                    continue;
                }

                step_messages.push_back({"user",
                                         "For repository-specific or code-analysis questions, you must call a tool before giving the final answer. "
                                         "Call the most relevant tool now, usually search_code first and then read_file if needed. Return JSON only."});
                trim_agent_messages(step_messages,
                                    fixed_prefix_count,
                                    kAgentModelInputMaxMessages,
                                    kAgentModelInputMaxChars);
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
    {
        ctx->error_message += ", last_output=" + truncate_text(last_model_output, 512);
    }
    ctx->EmitFinish(FinishReason::error);
}
