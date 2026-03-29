#include "serving/core/agent/AgentPrompt.h"

#include <sstream>

namespace
{
std::string join_tools(const std::vector<std::string> &allowed_tools)
{
    std::ostringstream tools_desc;
    tools_desc << "[";
    for (size_t i = 0; i < allowed_tools.size(); ++i)
    {
        if (i > 0)
            tools_desc << ", ";
        tools_desc << allowed_tools[i];
    }
    tools_desc << "]";
    return tools_desc.str();
}
} // namespace

std::string BuildToolPrompt(const std::string &agent_mode,
                            const std::vector<std::string> &allowed_tools)
{
    const std::string tools_desc = join_tools(allowed_tools);
    std::ostringstream oss;

    if (agent_mode == "code_analysis")
    {
        oss << "You are a read-only code analysis agent inside EdgeLLM-Serving.\n";
        oss << "Your job is to inspect the current repository and answer repository-specific questions with evidence.\n";
        oss << "Available tools: " << tools_desc << ".\n";
        oss << "If the user asks about code structure, files, functions, classes, configs, docs, architecture, models, APIs, or repository facts, you must call a tool before answering.\n";
        oss << "For code-flow questions, do not finalize after only one broad search result unless the evidence is already explicit.\n";
        oss << "If search_code returns candidate files, you should usually call read_file on the most relevant file before giving the final answer.\n";
        oss << "If the user asks a general question unrelated to this repository, answer directly without tools.\n";
        oss << "Never claim to have read code unless you used a tool.\n";
        oss << "Never mention function names, methods, classes, or call chains that were not present in tool output.\n";
        oss << "Never request file writes, patches, or shell execution.\n";
        oss << "Return JSON only.\n";
        oss << "Do not output <think>, </think>, chain-of-thought, or any hidden reasoning.\n";
        oss << "If a tool is needed, return:\n";
        oss << "{\"action\":\"tool\",\"tool\":\"tool_name\",\"input\":{...}}\n";
        oss << "If you can answer now, return:\n";
        oss << "{\"action\":\"final\",\"answer\":\"...\"}\n";
        oss << "The answer field should be a plain string, not an array or nested object.\n";
        oss << "Final answer rules:\n";
        oss << "- Keep the answer concise.\n";
        oss << "- Use at most 3 short bullets.\n";
        oss << "- Keep total output under about 120 English words (or about 180 Chinese characters).\n";
        oss << "- Cite concrete file paths when relevant.\n";
        oss << "- If evidence is incomplete, say so explicitly.\n";
        oss << "Tool usage rules:\n";
        oss << "- search_code: use for symbols, functions, strings, or call sites. Input should contain query and may contain path or limit.\n";
        oss << "- read_file: use for exact file inspection. Input should contain path and may contain start_line/end_line.\n";
        oss << "- list_files: use for directory structure. Input may contain path or limit.\n";
        oss << "- search_docs: use for README/docs answers. Input should contain query.\n";
        oss << "- get_config: use for config.json lookups. Input may contain key.\n";
        oss << "- get_server_status: use for runtime status. Input can be empty.\n";
        oss << "Final answers should be based only on observed tool evidence.\n";
        return oss.str();
    }

    oss << "You are an agent inside EdgeLLM-Serving.\n";
    oss << "You may either answer directly or call one tool.\n";
    oss << "Available tools: " << tools_desc << ".\n";
    oss << "If the user asks about server status, metrics, configuration, defaults, models, APIs, docs, architecture, files, or any repository-specific fact, you must call a tool before answering.\n";
    oss << "Do not guess repository facts from memory.\n";
    oss << "Return JSON only.\n";
    oss << "Do not output <think>, </think>, chain-of-thought, or any hidden reasoning.\n";
    oss << "If a tool is needed, return:\n";
    oss << "{\"action\":\"tool\",\"tool\":\"tool_name\",\"input\":{\"query\":\"...\"}}\n";
    oss << "If you can answer now, return:\n";
    oss << "{\"action\":\"final\",\"answer\":\"...\"}\n";
    oss << "Never wrap JSON in markdown.\n";
    oss << "Tool usage rules:\n";
    oss << "- search_docs: input should contain query.\n";
    oss << "- get_server_status: input can be empty.\n";
    oss << "- get_config: input can contain key.\n";
    return oss.str();
}

std::string BuildToolResultMessage(const std::string &tool_name, const std::string &tool_output)
{
    std::ostringstream oss;
    oss << "TOOL_RESULT name=" << tool_name << "\n";
    oss << tool_output << "\n";
    oss << "Based on this tool result, return JSON only for the next action.\n";
    oss << "If the evidence is still insufficient, call another tool instead of guessing.\n";
    oss << "If this was a search result, prefer read_file on the most relevant file before finalizing.\n";
    oss << "Do not invent symbols or call chains that are absent from the tool output.";
    return oss.str();
}
