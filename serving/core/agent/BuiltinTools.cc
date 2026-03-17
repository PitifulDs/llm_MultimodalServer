#include "serving/core/agent/BuiltinTools.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::string to_lower_copy(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return out;
}

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream in(path);
    if (!in.is_open())
        return "";

    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

std::string truncate_text(const std::string &text, size_t max_chars)
{
    if (text.size() <= max_chars)
        return text;

    if (max_chars <= 32)
        return text.substr(0, max_chars);

    return text.substr(0, max_chars) + "\n...[truncated]";
}

std::vector<std::string> split_terms(const std::string &query)
{
    std::vector<std::string> terms;
    std::istringstream iss(query);
    std::string term;
    while (iss >> term)
    {
        term = to_lower_copy(term);
        if (!term.empty())
            terms.push_back(term);
    }
    return terms;
}

std::string get_query_value(const nlohmann::json &input, const char *key)
{
    if (input.is_string())
        return input.get<std::string>();
    if (input.is_object() && input.contains(key) && input[key].is_string())
        return input[key].get<std::string>();
    return "";
}

std::string json_to_string(const nlohmann::json &j)
{
    if (j.is_string())
        return j.get<std::string>();
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string search_docs_tool(const BuiltinToolsOptions &options, const nlohmann::json &input)
{
    const std::string query = trim_copy(get_query_value(input, "query"));
    if (query.empty())
        return "search_docs requires a non-empty query.";

    std::vector<std::filesystem::path> files;
    const std::filesystem::path root(options.docs_root);
    files.push_back(root / "README.md");
    files.push_back(root / "serving" / "http" / "README.md");
    files.push_back(root / "docs" / "ARCHITECTURE.md");
    files.push_back(root / "docs" / "API_EXAMPLES.md");
    files.push_back(root / "docs" / "DESIGN_PATTERNS.md");
    files.push_back(root / "docs" / "PROJECT_HIGHLIGHTS.md");

    const std::vector<std::string> terms = split_terms(query);
    if (terms.empty())
        return "search_docs query is empty after normalization.";

    struct Match
    {
        std::filesystem::path path;
        int score = 0;
        std::string snippet;
    };

    std::vector<Match> matches;
    for (const auto &path : files)
    {
        const std::string content = read_file(path);
        if (content.empty())
            continue;

        const std::string lower = to_lower_copy(content);
        int score = 0;
        size_t first_pos = std::string::npos;
        for (const auto &term : terms)
        {
            const auto pos = lower.find(term);
            if (pos != std::string::npos)
            {
                ++score;
                if (first_pos == std::string::npos || pos < first_pos)
                    first_pos = pos;
            }
        }

        if (score == 0)
            continue;

        const size_t begin = (first_pos == std::string::npos || first_pos < 140) ? 0 : first_pos - 140;
        const size_t len = std::min<size_t>(320, content.size() - begin);
        std::string snippet = content.substr(begin, len);
        std::replace(snippet.begin(), snippet.end(), '\n', ' ');
        matches.push_back({path, score, snippet});
    }

    if (matches.empty())
    {
        return "No documentation match was found for query: " + query;
    }

    std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b)
              {
                  if (a.score != b.score)
                      return a.score > b.score;
                  return a.path.string() < b.path.string();
              });

    std::ostringstream oss;
    oss << "Top documentation matches for query: " << query << "\n";
    const size_t limit = std::min<size_t>(3, matches.size());
    for (size_t i = 0; i < limit; ++i)
    {
        oss << "- file=" << matches[i].path.string() << " score=" << matches[i].score << "\n";
        oss << "  snippet=" << matches[i].snippet << "\n";
    }
    return truncate_text(oss.str(), options.max_tool_output_chars);
}

std::string get_config_tool(const BuiltinToolsOptions &options, const nlohmann::json &input)
{
    const std::filesystem::path config_path(options.config_path);
    const std::string raw = read_file(config_path);
    if (raw.empty())
        return "config file not found: " + config_path.string();

    try
    {
        const nlohmann::json cfg = nlohmann::json::parse(raw);
        const std::string key = trim_copy(get_query_value(input, "key"));
        if (!key.empty())
        {
            if (!cfg.contains(key))
                return "config key not found: " + key;
            return std::string("config[") + key + "]=" + json_to_string(cfg[key]);
        }

        return truncate_text(cfg.dump(2), options.max_tool_output_chars);
    }
    catch (const std::exception &e)
    {
        return std::string("failed to parse config: ") + e.what();
    }
}
} // namespace

void RegisterBuiltinTools(ToolRegistry &registry,
                          const BuiltinToolsOptions &options,
                          std::function<std::string()> status_provider)
{
    registry.Register("search_docs", [options](const nlohmann::json &input)
                      { return search_docs_tool(options, input); });

    registry.Register("get_config", [options](const nlohmann::json &input)
                      { return get_config_tool(options, input); });

    registry.Register("get_server_status", [options, status_provider](const nlohmann::json &input)
                      {
                          (void)input;
                          if (!status_provider)
                              return truncate_text(std::string("server status provider is unavailable."), options.max_tool_output_chars);
                          return truncate_text(status_provider(), options.max_tool_output_chars);
                      });
}
