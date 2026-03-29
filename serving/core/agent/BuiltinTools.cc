#include "serving/core/agent/BuiltinTools.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
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

std::string trim_token_edges(std::string token)
{
    auto is_token_char = [](unsigned char ch)
    {
        return std::isalnum(ch) != 0 || ch == '_' || ch == ':' || ch == '-';
    };

    while (!token.empty() && !is_token_char(static_cast<unsigned char>(token.front())))
        token.erase(token.begin());
    while (!token.empty() && !is_token_char(static_cast<unsigned char>(token.back())))
        token.pop_back();
    return token;
}

bool looks_like_code_symbol(const std::string &token)
{
    for (unsigned char ch : token)
    {
        if (std::isupper(ch) != 0 || ch == '_' || ch == ':')
            return true;
    }
    return false;
}

std::vector<std::string> split_code_terms(const std::string &query)
{
    std::vector<std::string> generic = split_terms(query);
    std::vector<std::string> preferred;
    std::istringstream iss(query);
    std::string token;
    while (iss >> token)
    {
        token = trim_token_edges(token);
        if (token.empty())
            continue;
        if (!looks_like_code_symbol(token))
            continue;
        preferred.push_back(to_lower_copy(token));
    }

    auto dedupe = [](std::vector<std::string> items)
    {
        std::vector<std::string> out;
        std::set<std::string> seen;
        for (const auto &item : items)
        {
            if (item.empty())
                continue;
            if (!seen.insert(item).second)
                continue;
            out.push_back(item);
        }
        return out;
    };

    preferred = dedupe(std::move(preferred));
    if (!preferred.empty())
        return preferred;
    return dedupe(std::move(generic));
}

std::string get_string_value(const nlohmann::json &input, const char *key)
{
    if (input.is_string())
        return input.get<std::string>();
    if (input.is_object() && input.contains(key) && input[key].is_string())
        return input[key].get<std::string>();
    return "";
}

std::string get_first_string_value(const nlohmann::json &input,
                                   std::initializer_list<const char *> keys,
                                   bool fallback_to_any_string = false)
{
    for (const auto *key : keys)
    {
        const std::string value = get_string_value(input, key);
        if (!value.empty())
            return value;
    }

    if (fallback_to_any_string && input.is_object())
    {
        for (auto it = input.begin(); it != input.end(); ++it)
        {
            if (it.value().is_string())
                return it.value().get<std::string>();
        }
    }

    return "";
}

int get_int_value(const nlohmann::json &input, const char *key, int fallback)
{
    if (input.is_object() && input.contains(key) && input[key].is_number_integer())
        return input[key].get<int>();
    return fallback;
}

std::string json_to_string(const nlohmann::json &j)
{
    if (j.is_string())
        return j.get<std::string>();
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::filesystem::path normalize_existing_root(const std::string &root)
{
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(std::filesystem::path(root), ec);
    if (ec)
        return std::filesystem::path(root).lexically_normal();
    return p;
}

bool is_within_root(const std::filesystem::path &root, const std::filesystem::path &candidate)
{
    auto root_it = root.begin();
    auto cand_it = candidate.begin();
    for (; root_it != root.end() && cand_it != candidate.end(); ++root_it, ++cand_it)
    {
        if (*root_it != *cand_it)
            return false;
    }
    return root_it == root.end();
}

std::filesystem::path resolve_repo_path(const BuiltinToolsOptions &options,
                                        const std::string &relative,
                                        bool *ok = nullptr)
{
    const auto root = normalize_existing_root(options.repo_root);
    const std::filesystem::path raw = relative.empty() ? std::filesystem::path(".") : std::filesystem::path(relative);
    const std::filesystem::path joined = raw.is_absolute() ? raw : (root / raw);

    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(joined, ec);
    if (ec)
        normalized = joined.lexically_normal();

    const bool inside = is_within_root(root, normalized);
    if (ok)
        *ok = inside;
    return normalized;
}

bool is_code_file(const std::filesystem::path &path)
{
    static const std::set<std::string> exts = {
        ".cc", ".cpp", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx",
        ".ipp", ".tpp", ".cmake", ".md", ".json", ".sh", ".txt"};
    const std::string ext = to_lower_copy(path.extension().string());
    return exts.find(ext) != exts.end() || path.filename() == "CMakeLists.txt";
}

struct DocMatch
{
    std::filesystem::path path;
    int score = 0;
    std::string snippet;
};

bool compare_doc_match(const DocMatch &a, const DocMatch &b)
{
    if (a.score != b.score)
        return a.score > b.score;
    return a.path.string() < b.path.string();
}

struct CodeMatch
{
    std::string file;
    int line = 0;
    int score = 0;
    std::string text;
};

bool compare_code_match(const CodeMatch &a, const CodeMatch &b)
{
    if (a.score != b.score)
        return a.score > b.score;
    if (a.file != b.file)
        return a.file < b.file;
    return a.line < b.line;
}

std::string search_docs_tool(const BuiltinToolsOptions &options, const nlohmann::json &input)
{
    const std::string query = trim_copy(get_first_string_value(input, {"query", "search", "keyword", "text", "pattern"}, true));
    if (query.empty())
        return "search_docs requires a non-empty query.";

    std::vector<std::filesystem::path> files;
    const std::filesystem::path root(options.docs_root);
    files.push_back(root / "README.md");
    files.push_back(root / "serving" / "http" / "使用说明.md");
    files.push_back(root / "docs" / "系统架构.md");
    files.push_back(root / "docs" / "API调用示例.md");
    files.push_back(root / "docs" / "设计模式.md");
    files.push_back(root / "docs" / "项目亮点.md");
    files.push_back(root / "docs" / "智能体使用说明.md");
    files.push_back(root / "agent" / "AGENT.md");

    const std::vector<std::string> terms = split_terms(query);
    if (terms.empty())
        return "search_docs query is empty after normalization.";

    std::vector<DocMatch> matches;
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
        return "No documentation match was found for query: " + query;

    std::sort(matches.begin(), matches.end(), compare_doc_match);

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
        const std::string key = trim_copy(get_first_string_value(input, {"key", "name", "config", "field"}));
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

std::string list_files_tool(const BuiltinToolsOptions &options, const nlohmann::json &input)
{
    const std::string rel_path = trim_copy(get_first_string_value(input, {"path", "dir", "directory", "folder"}));
    const int limit = std::max(1, std::min(get_int_value(input, "limit", 80), 200));

    bool ok = false;
    const auto root = normalize_existing_root(options.repo_root);
    const auto target = resolve_repo_path(options, rel_path, &ok);
    if (!ok)
        return "list_files path is outside repository root.";
    if (!std::filesystem::exists(target))
        return "list_files target does not exist: " + target.string();
    if (!std::filesystem::is_directory(target))
        return "list_files target is not a directory: " + target.string();

    std::vector<std::string> items;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(target, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        const auto &entry = *it;
        const auto rel = std::filesystem::relative(entry.path(), root, ec);
        if (ec)
            continue;
        items.push_back(rel.string() + (entry.is_directory() ? "/" : ""));
        if (static_cast<int>(items.size()) >= limit)
            break;
    }

    if (items.empty())
        return "list_files found no entries.";

    std::ostringstream oss;
    oss << "Files under " << (rel_path.empty() ? "." : rel_path) << "\n";
    for (const auto &item : items)
        oss << "- " << item << "\n";
    return truncate_text(oss.str(), options.max_tool_output_chars);
}

std::string read_file_tool(const BuiltinToolsOptions &options, const nlohmann::json &input)
{
    const std::string rel_path = trim_copy(get_first_string_value(input, {"path", "file", "filepath", "filename"}));
    if (rel_path.empty())
        return "read_file requires path.";

    bool ok = false;
    const auto root = normalize_existing_root(options.repo_root);
    const auto target = resolve_repo_path(options, rel_path, &ok);
    if (!ok)
        return "read_file path is outside repository root.";
    if (!std::filesystem::exists(target))
        return "read_file target does not exist: " + target.string();
    if (std::filesystem::is_directory(target))
        return "read_file target is a directory: " + target.string();

    const int start_line = std::max(1, get_int_value(input, "start_line", 1));
    const int end_line = std::max(start_line, get_int_value(input, "end_line", start_line + 119));

    std::ifstream in(target);
    if (!in.is_open())
        return "failed to open file: " + target.string();

    std::ostringstream oss;
    oss << "FILE " << rel_path << " lines " << start_line << "-" << end_line << "\n";
    std::string line;
    int lineno = 0;
    while (std::getline(in, line))
    {
        ++lineno;
        if (lineno < start_line)
            continue;
        if (lineno > end_line)
            break;
        oss << lineno << ": " << line << "\n";
    }
    return truncate_text(oss.str(), options.max_tool_output_chars);
}

std::string search_code_tool(const BuiltinToolsOptions &options, const nlohmann::json &input)
{
    const std::string query = trim_copy(get_first_string_value(input, {"query", "search", "keyword", "text", "pattern"}, true));
    if (query.empty())
        return "search_code requires a non-empty query.";

    const std::string rel_path = trim_copy(get_first_string_value(input, {"path", "file", "filepath", "filename", "dir", "directory"}));
    const int limit = std::max(1, std::min(get_int_value(input, "limit", 8), 50));

    bool ok = false;
    const auto root = normalize_existing_root(options.repo_root);
    auto target = resolve_repo_path(options, rel_path, &ok);
    if (!ok)
        return "search_code path is outside repository root.";

    bool fallback_to_root = false;
    if (!std::filesystem::exists(target))
    {
        if (rel_path.empty())
            return "search_code target does not exist: " + target.string();
        target = root;
        fallback_to_root = true;
    }

    const std::vector<std::string> terms = split_code_terms(query);
    if (terms.empty())
        return "search_code query is empty after normalization.";

    std::vector<CodeMatch> matches;

    auto scan_file = [&](const std::filesystem::path &path)
    {
        if (!is_code_file(path))
            return;
        std::ifstream in(path);
        if (!in.is_open())
            return;
        std::string line;
        int lineno = 0;
        while (std::getline(in, line))
        {
            ++lineno;
            const std::string lower_line = to_lower_copy(line);
            int score = 0;
            for (const auto &term : terms)
            {
                if (lower_line.find(term) != std::string::npos)
                    ++score;
            }
            if (score == 0)
                continue;
            std::error_code ec;
            auto rel = std::filesystem::relative(path, root, ec);
            matches.push_back({ec ? path.string() : rel.string(), lineno, score, line});
        }
    };

    std::error_code ec;
    if (std::filesystem::is_regular_file(target))
    {
        scan_file(target);
    }
    else
    {
        for (auto it = std::filesystem::recursive_directory_iterator(target, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (it->is_regular_file())
                scan_file(it->path());
            if (static_cast<int>(matches.size()) >= limit)
                break;
        }
    }

    if (matches.empty())
    {
        if (fallback_to_root)
            return "search_code warning: path '" + rel_path + "' does not exist; searched repository root instead.\n"
                   "No code match was found for query: " + query;
        return "No code match was found for query: " + query;
    }

    std::sort(matches.begin(), matches.end(), compare_code_match);

    if (static_cast<int>(matches.size()) > limit)
        matches.resize(limit);

    std::ostringstream oss;
    if (fallback_to_root)
    {
        oss << "search_code warning: path '" << rel_path
            << "' does not exist; searched repository root instead.\n";
    }
    oss << "Code matches for query: " << query << "\n";
    for (const auto &m : matches)
        oss << "- file=" << m.file << ":" << m.line << " score=" << m.score << " text=" << m.text << "\n";
    return truncate_text(oss.str(), options.max_tool_output_chars);
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

    registry.Register("search_code", [options](const nlohmann::json &input)
                      { return search_code_tool(options, input); });

    registry.Register("read_file", [options](const nlohmann::json &input)
                      { return read_file_tool(options, input); });

    registry.Register("list_files", [options](const nlohmann::json &input)
                      { return list_files_tool(options, input); });
}
