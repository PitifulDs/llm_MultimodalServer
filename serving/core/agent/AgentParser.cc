#include "serving/core/agent/AgentParser.h"

#include <cctype>
#include <sstream>
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
    const std::string think_start = "<think>";
    const std::string think_end = "</think>";
    while (true)
    {
        const auto start = raw.find(think_start);
        if (start == std::string::npos)
            break;

        const auto end = raw.find(think_end, start + think_start.size());
        if (end == std::string::npos)
        {
            raw.erase(start);
            break;
        }

        raw.erase(start, end + think_end.size() - start);
    }

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
    if (j.is_array())
    {
        std::ostringstream oss;
        bool first = true;
        for (const auto &item : j)
        {
            const std::string piece = json_to_string(item);
            if (piece.empty())
                continue;
            if (!first)
                oss << "\n";
            oss << piece;
            first = false;
        }
        return oss.str();
    }
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

bool extract_relaxed_json_int_field(const std::string &raw,
                                    const std::string &key,
                                    int &out)
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
    if (pos >= raw.size())
        return false;

    bool neg = false;
    if (raw[pos] == '-')
    {
        neg = true;
        ++pos;
    }

    if (pos >= raw.size() || !std::isdigit(static_cast<unsigned char>(raw[pos])))
        return false;

    int value = 0;
    for (; pos < raw.size(); ++pos)
    {
        const unsigned char ch = static_cast<unsigned char>(raw[pos]);
        if (!std::isdigit(ch))
            break;
        value = value * 10 + static_cast<int>(ch - '0');
    }
    out = neg ? -value : value;
    return true;
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
        auto get_tool_name = [&]() -> std::string
        {
            if (kind == "tool")
                return parsed.value("tool", "");
            if (!kind.empty() && kind != "final" && kind != "answer")
                return kind;
            if (kind.empty())
                return parsed.value("tool", "");
            return {};
        };

        const std::string tool_name = get_tool_name();
        if (!tool_name.empty())
        {
            action.type = AgentAction::Type::tool_call;
            action.tool_name = tool_name;
            if (parsed.contains("input"))
            {
                action.tool_input = parsed["input"];
            }
            else
            {
                nlohmann::json input = nlohmann::json::object();
                for (auto it = parsed.begin(); it != parsed.end(); ++it)
                {
                    const std::string key = it.key();
                    if (key == "action" || key == "tool" || key == "answer" || key == "reason")
                        continue;
                    input[key] = it.value();
                }

                if (parsed.contains("file_path") && !input.contains("path"))
                    input["path"] = parsed["file_path"];
                if (parsed.contains("filepath") && !input.contains("path"))
                    input["path"] = parsed["filepath"];
                if (parsed.contains("directory") && !input.contains("path"))
                    input["path"] = parsed["directory"];

                action.tool_input = std::move(input);
            }
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

    // Relaxed tool-call extraction for truncated/invalid JSON outputs.
    std::string relaxed_action;
    std::string relaxed_tool;
    extract_relaxed_json_string_field(normalized, "action", relaxed_action);
    extract_relaxed_json_string_field(normalized, "tool", relaxed_tool);

    std::string inferred_tool = trim_copy(relaxed_tool);
    if (inferred_tool.empty())
    {
        const std::string kind = trim_copy(relaxed_action);
        if (!kind.empty() && kind != "tool" && kind != "final" && kind != "answer")
            inferred_tool = kind;
    }

    if (!inferred_tool.empty())
    {
        action.type = AgentAction::Type::tool_call;
        action.tool_name = inferred_tool;
        action.tool_input = nlohmann::json::object();

        std::string value;
        if (extract_relaxed_json_string_field(normalized, "path", value))
            action.tool_input["path"] = value;
        if (extract_relaxed_json_string_field(normalized, "file_path", value) && !action.tool_input.contains("path"))
            action.tool_input["path"] = value;
        if (extract_relaxed_json_string_field(normalized, "filepath", value) && !action.tool_input.contains("path"))
            action.tool_input["path"] = value;
        if (extract_relaxed_json_string_field(normalized, "directory", value) && !action.tool_input.contains("path"))
            action.tool_input["path"] = value;
        if (extract_relaxed_json_string_field(normalized, "query", value))
            action.tool_input["query"] = value;
        if (extract_relaxed_json_string_field(normalized, "search", value) && !action.tool_input.contains("query"))
            action.tool_input["query"] = value;
        if (extract_relaxed_json_string_field(normalized, "keyword", value) && !action.tool_input.contains("query"))
            action.tool_input["query"] = value;

        int number = 0;
        if (extract_relaxed_json_int_field(normalized, "start_line", number))
            action.tool_input["start_line"] = number;
        if (extract_relaxed_json_int_field(normalized, "end_line", number))
            action.tool_input["end_line"] = number;
        if (extract_relaxed_json_int_field(normalized, "limit", number))
            action.tool_input["limit"] = number;

        return action;
    }

    return action;
}
