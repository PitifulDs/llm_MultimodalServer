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
        oss << "- search_kb: use first for repository questions that may benefit from RAG over docs/repo_code. Input should contain kb, query, and may contain top_k or mode.\n";
        oss << "- open_chunk: use after search_kb when you need the full retrieved chunk. Input should contain chunk_id.\n";
        oss << "- search_code: use for symbols, functions, strings, or call sites. Input should contain query and may contain path or limit.\n";
        oss << "- read_file: use for exact file inspection. Input should contain path and may contain start_line/end_line.\n";
        oss << "- list_files: use for directory structure. Input may contain path or limit.\n";
        oss << "- search_docs: use for README/docs answers. Input should contain query.\n";
        oss << "- get_config: use for config.json lookups. Input may contain key.\n";
        oss << "- get_server_status: use for runtime status. Input can be empty.\n";
        oss << "Final answers should be based only on observed tool evidence.\n";
        return oss.str();
    }

    if (agent_mode == "web_research")
    {
        oss << "You are a read-only hybrid research agent inside EdgeLLM-Serving.\n";
        oss << "Your job is to combine repository evidence and controlled web evidence.\n";
        oss << "Available tools: " << tools_desc << ".\n";
        oss << "Prefer local repository tools for repo-specific facts, and use search_web / fetch_url only for external or recent information.\n";
        oss << "Never use shell, curl, or any capability outside the provided tools.\n";
        oss << "Return JSON only.\n";
        oss << "If a tool is needed, return:\n";
        oss << "{\"action\":\"tool\",\"tool\":\"tool_name\",\"input\":{...}}\n";
        oss << "If you can answer now, return:\n";
        oss << "{\"action\":\"final\",\"answer\":\"...\"}\n";
        oss << "Tool usage rules:\n";
        oss << "- search_kb/open_chunk/search_code/read_file/search_docs: repository and documentation evidence.\n";
        oss << "- search_web: external search results only. Input should contain query and may contain top_k.\n";
        oss << "- fetch_url: fetch one public webpage and extract正文. Input should contain url.\n";
        return oss.str();
    }

    oss << "You are an internal experimental agent inside EdgeLLM-Serving.\n";
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
    oss << "- search_kb: input should contain kb and query.\n";
    oss << "- open_chunk: input should contain chunk_id.\n";
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

std::string BuildWebResearchDecompositionPrompt()
{
    std::ostringstream oss;
    oss << "You are a read-only research planning model inside EdgeLLM-Serving.\n";
    oss << "Your only job is to decompose the user's question into 3 to 5 research subqueries.\n";
    oss << "Do not call tools. Do not describe shell, curl, HTTP clients, or execution details.\n";
    oss << "For each subquery, choose exactly one source from: local, web, hybrid.\n";
    oss << "Use local for repository/docs questions, web for external-only facts, and hybrid when both local and web evidence are needed.\n";
    oss << "Prefer short, specific queries that preserve symbols, file names, APIs, versions, or time-sensitive terms from the question.\n";
    oss << "Return JSON only in this shape:\n";
    oss << "{\"subqueries\":[{\"label\":\"...\",\"query\":\"...\",\"source\":\"local|web|hybrid\"}]}\n";
    oss << "Never include markdown fences or extra commentary.";
    return oss.str();
}

std::string BuildWebResearchSynthesisPrompt()
{
    std::ostringstream oss;
    oss << "You are a read-only research synthesis model inside EdgeLLM-Serving.\n";
    oss << "You will receive a user question plus collected evidence. Use only that evidence.\n";
    oss << "Do not call tools. Do not invent facts. If evidence is incomplete, state the gap.\n";
    oss << "Return JSON only in this shape:\n";
    oss << "{\"summary\":\"...\",\"analysis\":[\"...\"],\"risks\":[\"...\"],\"next_steps\":[\"...\"],\"references\":[\"E1\",\"E2\"]}\n";
    oss << "references must contain evidence ids from the provided list.\n";
    oss << "Keep summary concise, analysis to 2-4 lines, risks to 0-3 lines, and next_steps to 0-2 lines.\n";
    oss << "Never include markdown fences or extra commentary.";
    return oss.str();
}
