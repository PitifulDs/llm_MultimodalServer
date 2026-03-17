#include "serving/core/agent/AgentPrompt.h"

#include <sstream>

std::string BuildToolPrompt(const std::vector<std::string> &allowed_tools)
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

    std::ostringstream oss;
    oss << "You are an agent inside EdgeLLM-Serving.\n";
    oss << "You may either answer directly or call one tool.\n";
    oss << "Available tools: " << tools_desc.str() << ".\n";
    oss << "If the user asks about server status, metrics, configuration, defaults, models, APIs, docs, architecture, files, or any repository-specific fact, you must call a tool before answering.\n";
    oss << "Do not guess repository facts from memory.\n";
    oss << "Return JSON only.\n";
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
    oss << "Based on this tool result, return JSON only for the next action.";
    return oss.str();
}
