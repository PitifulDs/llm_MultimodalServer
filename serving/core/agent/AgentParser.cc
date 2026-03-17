#include "serving/core/agent/AgentParser.h"

#include <cctype>
#include <string>
#include <utility>

namespace
{
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

std::string normalize_model_json(std::string raw)
{
    raw = trim_copy(std::move(raw));

    const std::string fence = "```";
    if (raw.rfind(fence, 0) == 0)
    {
        const auto first_nl = raw.find('\n');
        const auto last_fence = raw.rfind(fence);
        if (first_nl != std::string::npos && last_fence != std::string::npos && last_fence > first_nl)
        {
            raw = raw.substr(first_nl + 1, last_fence - first_nl - 1);
        }
    }

    const auto first = raw.find('{');
    const auto last = raw.rfind('}');
    if (first != std::string::npos && last != std::string::npos && first < last)
    {
        raw = raw.substr(first, last - first + 1);
    }

    return trim_copy(std::move(raw));
}

std::string json_to_string(const nlohmann::json &j)
{
    if (j.is_string())
        return j.get<std::string>();
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
} // namespace

AgentAction ParseAgentAction(const std::string &raw_output)
{
    AgentAction action;
    action.answer = trim_copy(raw_output);

    const std::string normalized = normalize_model_json(raw_output);
    if (normalized.empty())
        return action;

    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(normalized);
        const std::string kind = parsed.value("action", "");
        if (kind == "tool")
        {
            action.type = AgentAction::Type::tool_call;
            action.tool_name = parsed.value("tool", "");
            if (parsed.contains("input"))
                action.tool_input = parsed["input"];
            return action;
        }

        if (parsed.contains("answer"))
        {
            action.type = AgentAction::Type::final_answer;
            action.answer = json_to_string(parsed["answer"]);
            return action;
        }
    }
    catch (...)
    {
    }

    return action;
}
