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

bool extract_relaxed_json_string_field(const std::string &raw,
                                       const std::string &key,
                                       std::string &out)
{
    const std::string marker = "\"" + key + "\"";
    const auto key_pos = raw.find(marker);
    if (key_pos == std::string::npos)
        return false;

    const auto colon_pos = raw.find(':', key_pos + marker.size());
    if (colon_pos == std::string::npos)
        return false;

    size_t pos = colon_pos + 1;
    while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos])) != 0)
        ++pos;
    if (pos >= raw.size() || raw[pos] != '"')
        return false;

    ++pos;
    std::string value;
    bool escape = false;
    for (; pos < raw.size(); ++pos)
    {
        const char ch = raw[pos];
        if (escape)
        {
            switch (ch)
            {
            case 'n':
                value.push_back('\n');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case '"':
            case '\\':
            case '/':
                value.push_back(ch);
                break;
            default:
                value.push_back(ch);
                break;
            }
            escape = false;
            continue;
        }

        if (ch == '\\')
        {
            escape = true;
            continue;
        }
        if (ch == '"')
        {
            out = value;
            return true;
        }
        value.push_back(ch);
    }
    return false;
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

    std::string relaxed_answer;
    if (extract_relaxed_json_string_field(normalized, "answer", relaxed_answer))
    {
        action.type = AgentAction::Type::final_answer;
        action.answer = trim_copy(std::move(relaxed_answer));
        return action;
    }

    return action;
}
