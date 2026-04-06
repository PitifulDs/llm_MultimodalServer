#include "serving/core/agent/BuiltinTools.h"
#include "thirds/llama.cpp/vendor/cpp-httplib/httplib.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <netdb.h>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <vector>

namespace
{
struct CommonHttpUrl
{
    std::string scheme;
    std::string user;
    std::string password;
    std::string host;
    int port = 0;
    std::string path;
};

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

CommonHttpUrl parse_http_url(const std::string &url)
{
    CommonHttpUrl parts;
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw std::runtime_error("invalid URL: no scheme");

    parts.scheme = url.substr(0, scheme_end);
    if (parts.scheme != "http" && parts.scheme != "https")
        throw std::runtime_error("unsupported URL scheme: " + parts.scheme);

    std::string rest = url.substr(scheme_end + 3);
    const auto at_pos = rest.find('@');
    if (at_pos != std::string::npos)
    {
        const std::string auth = rest.substr(0, at_pos);
        const auto colon_pos = auth.find(':');
        if (colon_pos != std::string::npos)
        {
            parts.user = auth.substr(0, colon_pos);
            parts.password = auth.substr(colon_pos + 1);
        }
        else
        {
            parts.user = auth;
        }
        rest = rest.substr(at_pos + 1);
    }

    const auto slash_pos = rest.find('/');
    if (slash_pos != std::string::npos)
    {
        parts.host = rest.substr(0, slash_pos);
        parts.path = rest.substr(slash_pos);
    }
    else
    {
        parts.host = rest;
        parts.path = "/";
    }

    const auto colon_pos = parts.host.rfind(':');
    if (colon_pos != std::string::npos && parts.host.find(']') == std::string::npos)
    {
        parts.port = std::stoi(parts.host.substr(colon_pos + 1));
        parts.host = parts.host.substr(0, colon_pos);
    }
    else
    {
        parts.port = parts.scheme == "https" ? 443 : 80;
    }

    return parts;
}

std::vector<std::string> split_terms(const std::string &query)
{
    std::vector<std::string> terms;
    std::istringstream iss(query);
    std::string term;
    static const std::set<std::string> stopwords = {
        "的", "是", "在", "里", "怎么", "如何", "哪里", "哪一层", "哪个", "什么",
        "and", "the", "is", "in", "of", "to", "for", "what", "where", "how"};
    while (iss >> term)
    {
        term = to_lower_copy(term);
        if (!term.empty() && stopwords.find(term) == stopwords.end())
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
    auto camel_to_snake = [](const std::string &token)
    {
        std::string out;
        for (size_t i = 0; i < token.size(); ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(token[i]);
            if (std::isupper(ch) != 0)
            {
                if (!out.empty())
                    out.push_back('_');
                out.push_back(static_cast<char>(std::tolower(ch)));
            }
            else
            {
                out.push_back(static_cast<char>(std::tolower(ch)));
            }
        }
        return out;
    };

    std::istringstream iss(query);
    std::string token;
    while (iss >> token)
    {
        token = trim_token_edges(token);
        if (token.empty())
            continue;
        if (!looks_like_code_symbol(token))
            continue;
        const std::string lower = to_lower_copy(token);
        preferred.push_back(lower);
        const std::string snake = camel_to_snake(token);
        if (!snake.empty() && snake != lower)
            preferred.push_back(snake);
        std::string compact;
        if (lower.find('_') != std::string::npos)
        {
            compact = lower;
            compact.erase(std::remove(compact.begin(), compact.end(), '_'), compact.end());
        }
        if (!compact.empty() && compact != lower)
            preferred.push_back(compact);
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

std::string url_encode_component(const std::string &value)
{
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char ch : value)
    {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            out.push_back(static_cast<char>(ch));
            continue;
        }
        if (ch == ' ')
        {
            out.push_back('+');
            continue;
        }
        out.push_back('%');
        out.push_back(kHex[(ch >> 4) & 0x0F]);
        out.push_back(kHex[ch & 0x0F]);
    }
    return out;
}

std::string url_decode_component(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    auto hex_value = [](unsigned char ch) -> int
    {
        if (ch >= '0' && ch <= '9')
            return static_cast<int>(ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<int>(ch - 'a' + 10);
        if (ch >= 'A' && ch <= 'F')
            return static_cast<int>(ch - 'A' + 10);
        return -1;
    };

    for (size_t i = 0; i < value.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch == '+' )
        {
            out.push_back(' ');
            continue;
        }
        if (ch == '%' && i + 2 < value.size())
        {
            const int hi = hex_value(static_cast<unsigned char>(value[i + 1]));
            const int lo = hex_value(static_cast<unsigned char>(value[i + 2]));
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

std::string html_decode_entities(const std::string &input)
{
    std::string out = input;
    auto replace_all = [](std::string &text, const std::string &from, const std::string &to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos)
        {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    replace_all(out, "&amp;", "&");
    replace_all(out, "&lt;", "<");
    replace_all(out, "&gt;", ">");
    replace_all(out, "&quot;", "\"");
    replace_all(out, "&#39;", "'");
    replace_all(out, "&nbsp;", " ");
    return out;
}

std::string collapse_spaces(std::string text)
{
    std::string out;
    out.reserve(text.size());
    bool prev_space = false;
    for (unsigned char ch : text)
    {
        if (std::isspace(ch) != 0)
        {
            if (!prev_space)
                out.push_back(' ');
            prev_space = true;
            continue;
        }
        out.push_back(static_cast<char>(ch));
        prev_space = false;
    }
    return trim_copy(std::move(out));
}

std::string strip_html_tags(std::string html)
{
    static const std::regex kRemoveBlocks(R"(<(script|style|noscript|svg|header|footer|nav|aside|form)[^>]*>.*?</\1>)",
                                          std::regex::icase | std::regex::optimize);
    static const std::regex kBreaks(R"(<\s*(br|/p|/div|/li|/tr|/h[1-6])\s*[^>]*>)",
                                    std::regex::icase | std::regex::optimize);
    static const std::regex kTags(R"(<[^>]+>)", std::regex::icase | std::regex::optimize);
    html = std::regex_replace(html, kRemoveBlocks, " ");
    html = std::regex_replace(html, kBreaks, "\n");
    html = std::regex_replace(html, kTags, " ");
    return collapse_spaces(html_decode_entities(html));
}

bool is_forbidden_literal_host(const std::string &host)
{
    in_addr ipv4{};
    if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1)
    {
        const uint32_t ip = ntohl(ipv4.s_addr);
        if ((ip >> 24) == 127 || (ip >> 24) == 10 || ip == 0)
            return true;
        if ((ip & 0xFFF00000u) == 0xAC100000u) // 172.16.0.0/12
            return true;
        if ((ip & 0xFFFF0000u) == 0xC0A80000u) // 192.168.0.0/16
            return true;
        if ((ip & 0xFFFF0000u) == 0xA9FE0000u) // 169.254.0.0/16
            return true;
        return false;
    }

    in6_addr ipv6{};
    if (inet_pton(AF_INET6, host.c_str(), &ipv6) == 1)
    {
        if (IN6_IS_ADDR_LOOPBACK(&ipv6) || IN6_IS_ADDR_LINKLOCAL(&ipv6) ||
            IN6_IS_ADDR_SITELOCAL(&ipv6) || IN6_IS_ADDR_UNSPECIFIED(&ipv6))
        {
            return true;
        }
        return false;
    }

    return false;
}

bool is_public_host(const std::string &host)
{
    const std::string lower = to_lower_copy(host);
    if (lower.empty() || lower == "localhost" || lower == "localhost.localdomain")
        return false;
    if (lower.find(".local") != std::string::npos || lower.find(".internal") != std::string::npos)
        return false;
    if (is_forbidden_literal_host(lower))
        return false;

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0)
        return false;

    bool allowed = false;
    for (addrinfo *rp = result; rp != nullptr; rp = rp->ai_next)
    {
        char buf[INET6_ADDRSTRLEN] = {0};
        if (rp->ai_family == AF_INET)
        {
            const auto *addr = reinterpret_cast<sockaddr_in *>(rp->ai_addr);
            if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)))
                continue;
        }
        else if (rp->ai_family == AF_INET6)
        {
            const auto *addr = reinterpret_cast<sockaddr_in6 *>(rp->ai_addr);
            if (!inet_ntop(AF_INET6, &addr->sin6_addr, buf, sizeof(buf)))
                continue;
        }
        else
        {
            continue;
        }

        if (!is_forbidden_literal_host(buf))
        {
            allowed = true;
            break;
        }
    }

    freeaddrinfo(result);
    return allowed;
}

bool is_safe_web_url(const std::string &url, std::string &error)
{
    try
    {
        const auto parts = parse_http_url(url);
        if (parts.scheme != "http" && parts.scheme != "https")
        {
            error = "unsupported URL scheme.";
            return false;
        }
        if (!parts.user.empty() || !parts.password.empty())
        {
            error = "credentials in URL are not allowed.";
            return false;
        }
        if (parts.port != 80 && parts.port != 443)
        {
            error = "only ports 80 and 443 are allowed.";
            return false;
        }
        if (!is_public_host(parts.host))
        {
            error = "target host is not allowed.";
            return false;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}

std::string shell_single_quote(const std::string &value)
{
    std::string out = "'";
    for (char ch : value)
    {
        if (ch == '\'')
            out += "'\\''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

bool curl_get_text(const std::string &url,
                   const std::string &user_agent,
                   std::string &body,
                   std::string &content_type,
                   std::string &error)
{
    const std::string marker = "__EDGE_CONTENT_TYPE__:";
    const std::string command =
        "curl -fsSL --connect-timeout 5 --max-time 12 "
        "-A " + shell_single_quote(user_agent) + " "
        "-H " + shell_single_quote("Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8") + " "
        "-H " + shell_single_quote("Accept-Language: en-US,en;q=0.8,zh-CN;q=0.6") + " "
        + shell_single_quote(url) + " "
        "-w " + shell_single_quote("\\n" + marker + "%{content_type}\\n");

    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe)
    {
        error = "curl fallback failed: popen returned null.";
        return false;
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        output.append(buffer);

    const int rc = pclose(pipe);
    if (rc != 0)
    {
        error = "curl fallback exited with status " + std::to_string(rc) + ".";
        return false;
    }

    const auto marker_pos = output.rfind(marker);
    if (marker_pos == std::string::npos)
    {
        error = "curl fallback missing content type marker.";
        return false;
    }

    body = output.substr(0, marker_pos);
    content_type = trim_copy(output.substr(marker_pos + marker.size()));
    if (!body.empty() && body.back() == '\n')
        body.pop_back();
    return true;
}

bool http_get_text(const std::string &url,
                   const std::string &user_agent,
                   std::string &body,
                   std::string &content_type,
                   std::string &error)
{
    try
    {
        const auto parts = parse_http_url(url);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
        if (parts.scheme == "https")
            return curl_get_text(url, user_agent, body, content_type, error);
#endif
        httplib::Client cli(parts.scheme + "://" + parts.host + ":" + std::to_string(parts.port));
        if (!parts.user.empty())
            cli.set_basic_auth(parts.user, parts.password);
        cli.set_follow_location(true);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(12, 0);
        cli.set_write_timeout(12, 0);
        httplib::Headers headers = {
            {"User-Agent", user_agent},
            {"Accept", "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8"},
            {"Accept-Language", "en-US,en;q=0.8,zh-CN;q=0.6"}};
        auto result = cli.Get(parts.path.c_str(), headers);
        if (!result)
        {
            error = "http request failed: " + httplib::to_string(result.error());
            return false;
        }
        if (result->status < 200 || result->status >= 300)
        {
            error = "http status=" + std::to_string(result->status);
            return false;
        }
        body = result->body;
        content_type = result->get_header_value("Content-Type");
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
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

std::string extract_symbol_from_line(const std::string &line)
{
    static const std::vector<std::string> prefixes = {
        "class ", "struct ", "enum class ", "enum ", "void ", "int ", "bool ",
        "std::string ", "size_t ", "static ", "inline ", "const "};

    std::string text = trim_copy(line);
    const auto paren = text.find('(');
    if (paren != std::string::npos)
    {
        const auto before = trim_copy(text.substr(0, paren));
        const auto sep = before.find_last_of(" :*&");
        return sep == std::string::npos ? before : before.substr(sep + 1);
    }

    for (const auto &prefix : prefixes)
    {
        if (text.rfind(prefix, 0) != 0)
            continue;
        text = text.substr(prefix.size());
        const auto end = text.find_first_of(" :{<");
        return end == std::string::npos ? text : text.substr(0, end);
    }
    return "";
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
    std::string symbol;
};

bool compare_code_match(const CodeMatch &a, const CodeMatch &b)
{
    if (a.score != b.score)
        return a.score > b.score;
    const bool a_impl = a.file.find(".cc") != std::string::npos || a.file.find(".cpp") != std::string::npos || a.file.find(".c:") != std::string::npos;
    const bool b_impl = b.file.find(".cc") != std::string::npos || b.file.find(".cpp") != std::string::npos || b.file.find(".c:") != std::string::npos;
    if (a_impl != b_impl)
        return a_impl;
    if (a.file != b.file)
        return a.file < b.file;
    return a.line < b.line;
}

struct WebSearchResult
{
    std::string title;
    std::string url;
    std::string snippet;
};

std::string extract_query_param(const std::string &url, const std::string &key)
{
    const std::string marker = key + "=";
    const auto pos = url.find(marker);
    if (pos == std::string::npos)
        return "";
    size_t end = url.find('&', pos + marker.size());
    if (end == std::string::npos)
        end = url.size();
    return url_decode_component(url.substr(pos + marker.size(), end - pos - marker.size()));
}

std::string normalize_web_url(std::string raw)
{
    raw = html_decode_entities(trim_copy(std::move(raw)));
    if (raw.rfind("//", 0) == 0)
        raw = "https:" + raw;
    const std::string lower = to_lower_copy(raw);
    if (lower.find("duckduckgo.com/l/?") != std::string::npos)
    {
        const std::string decoded = extract_query_param(raw, "uddg");
        if (!decoded.empty())
            raw = decoded;
    }
    return raw;
}

std::string extract_first_http_url(const std::string &text)
{
    static const std::regex kUrlRe(R"((https?://[^\s<>"']+))",
                                   std::regex::icase | std::regex::optimize);
    std::smatch match;
    if (!std::regex_search(text, match, kUrlRe) || match.size() < 2)
        return "";

    std::string url = match[1].str();
    while (!url.empty())
    {
        const char tail = url.back();
        if (tail == '.' || tail == ',' || tail == ';' || tail == ')' || tail == ']' || tail == '}' || tail == '"' || tail == '\'')
            url.pop_back();
        else
            break;
    }
    return normalize_web_url(url);
}

std::vector<WebSearchResult> parse_duckduckgo_lite_results(const std::string &html, int limit)
{
    std::vector<WebSearchResult> results;
    static const std::regex kLinkRe(R"(<a[^>]*class=['"]result-link['"][^>]*href=['"]([^'"]+)['"][^>]*>(.*?)</a>)",
                                    std::regex::icase | std::regex::optimize);
    static const std::regex kSnippetRe(R"(<td[^>]*class=['"]result-snippet['"][^>]*>(.*?)</td>)",
                                       std::regex::icase | std::regex::optimize);
    static const std::regex kLinkTextRe(R"(<span[^>]*class=['"]link-text['"][^>]*>(.*?)</span>)",
                                        std::regex::icase | std::regex::optimize);

    for (std::sregex_iterator it(html.begin(), html.end(), kLinkRe), end; it != end; ++it)
    {
        WebSearchResult item;
        item.url = normalize_web_url((*it)[1].str());
        item.title = collapse_spaces(strip_html_tags((*it)[2].str()));
        if (item.url.empty() || item.title.empty())
            continue;

        const size_t tail_start = static_cast<size_t>(it->position() + it->length());
        const std::string tail = html.substr(tail_start, std::min<size_t>(1400, html.size() - tail_start));
        std::smatch snippet_match;
        if (std::regex_search(tail, snippet_match, kSnippetRe))
            item.snippet = collapse_spaces(strip_html_tags(snippet_match[1].str()));
        if (item.snippet.empty())
        {
            std::smatch link_text_match;
            if (std::regex_search(tail, link_text_match, kLinkTextRe))
                item.snippet = collapse_spaces(strip_html_tags(link_text_match[1].str()));
        }

        results.push_back(std::move(item));
        if (static_cast<int>(results.size()) >= limit)
            break;
    }

    return results;
}

std::string extract_html_match(const std::string &html, const std::regex &pattern)
{
    std::smatch match;
    if (!std::regex_search(html, match, pattern) || match.size() < 2)
        return "";
    return collapse_spaces(strip_html_tags(match[1].str()));
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

std::string search_web_tool(const BuiltinToolsOptions &options, const nlohmann::json &input)
{
    const std::string query = trim_copy(get_first_string_value(input, {"query", "search", "keyword", "text", "pattern"}, true));
    if (query.empty())
        return "search_web requires a non-empty query.";

    const std::string direct_url = extract_first_http_url(query);
    if (!direct_url.empty())
    {
        std::string error;
        if (!is_safe_web_url(direct_url, error))
            return "search_web blocked: " + error;

        std::string title = direct_url;
        try
        {
            const auto parts = parse_http_url(direct_url);
            title = parts.host.empty() ? direct_url : parts.host + parts.path;
        }
        catch (...)
        {
        }

        std::ostringstream oss;
        oss << "Web search hits for query: " << query << "\n";
        oss << "- title=" << title << " url=" << direct_url << "\n";
        oss << "  snippet=Direct URL supplied in query; use fetch_url to collect webpage evidence.\n";
        return truncate_text(oss.str(), options.max_tool_output_chars);
    }

    const int top_k = std::max(1, std::min(get_int_value(input, "top_k", 5), 8));
    const std::string endpoint = "https://lite.duckduckgo.com/lite/?q=" + url_encode_component(query);

    std::string body;
    std::string content_type;
    std::string error;
    if (!http_get_text(endpoint, "EdgeLLM-Serving/agent-web-research", body, content_type, error))
        return "search_web failed: " + error;

    const auto results = parse_duckduckgo_lite_results(body, top_k);
    if (results.empty())
        return "No web result was found for query: " + query;

    std::ostringstream oss;
    oss << "Web search hits for query: " << query << "\n";
    for (const auto &item : results)
    {
        oss << "- title=" << item.title << " url=" << item.url << "\n";
        if (!item.snippet.empty())
            oss << "  snippet=" << item.snippet << "\n";
    }
    return truncate_text(oss.str(), options.max_tool_output_chars);
}

std::string fetch_url_tool(const BuiltinToolsOptions &options, const nlohmann::json &input)
{
    const std::string url = trim_copy(get_first_string_value(input, {"url", "href", "link"}, true));
    if (url.empty())
        return "fetch_url requires url.";

    std::string error;
    if (!is_safe_web_url(url, error))
        return "fetch_url blocked: " + error;

    std::string body;
    std::string content_type;
    if (!http_get_text(url, "EdgeLLM-Serving/agent-web-research", body, content_type, error))
        return "fetch_url failed: " + error;

    const std::string lower_type = to_lower_copy(content_type);
    std::string title;
    std::string canonical_url = url;
    std::string text;

    if (lower_type.find("text/html") != std::string::npos || lower_type.find("application/xhtml+xml") != std::string::npos || lower_type.empty())
    {
        static const std::regex kTitleRe(R"(<title[^>]*>(.*?)</title>)", std::regex::icase | std::regex::optimize);
        static const std::regex kOgTitleRe(R"(<meta[^>]+property=['"]og:title['"][^>]+content=['"]([^'"]+)['"])",
                                           std::regex::icase | std::regex::optimize);
        static const std::regex kCanonicalRe(R"(<link[^>]+rel=['"][^'"]*canonical[^'"]*['"][^>]+href=['"]([^'"]+)['"])",
                                             std::regex::icase | std::regex::optimize);

        title = extract_html_match(body, kTitleRe);
        if (title.empty())
            title = extract_html_match(body, kOgTitleRe);
        {
            std::smatch match;
            if (std::regex_search(body, match, kCanonicalRe) && match.size() >= 2)
                canonical_url = normalize_web_url(match[1].str());
        }
        text = strip_html_tags(body);
    }
    else
    {
        text = collapse_spaces(body);
    }

    title = trim_copy(title);
    if (title.empty())
        title = canonical_url;
    if (canonical_url.empty())
        canonical_url = url;
    if (text.empty())
        return "fetch_url failed: extracted page text is empty.";

    std::ostringstream oss;
    oss << "WEB_PAGE\n";
    oss << "title=" << title << "\n";
    oss << "canonical_url=" << canonical_url << "\n";
    oss << "url=" << url << "\n";
    oss << "text=" << truncate_text(text, std::min<size_t>(options.max_tool_output_chars, static_cast<size_t>(3200)));
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
            std::error_code rel_ec;
            const auto rel_path = std::filesystem::relative(path, root, rel_ec);
            const std::string rel = rel_ec ? path.string() : rel_path.string();
            const std::string lower_rel = to_lower_copy(rel);
            int score = 0;
            int matched_terms = 0;
            for (const auto &term : terms)
            {
                if (term.empty())
                    continue;
                const bool line_hit = lower_line.find(term) != std::string::npos;
                const bool path_hit = lower_rel.find(term) != std::string::npos;
                const bool base_hit = to_lower_copy(path.filename().string()).find(term) != std::string::npos;
                if (line_hit || path_hit)
                {
                    ++matched_terms;
                    score += 3;
                    if (line_hit)
                        score += 2;
                    if (path_hit)
                        score += 2;
                }
                if (base_hit)
                    score += 4;
            }
            if (score == 0)
                continue;

            if (matched_terms == static_cast<int>(terms.size()) && !terms.empty())
                score += 4;
            if (lower_line.find(to_lower_copy(query)) != std::string::npos)
                score += 6;
            if (lower_rel.find("serving/") == 0)
                score += 3;
            if (lower_rel.find("serving/http/") == 0 || lower_rel.find("serving/core/agent/") == 0 || lower_rel.find("serving/rag/") == 0)
                score += 2;
            if (lower_rel.find("thirds/") == 0 && query.find("thirds") == std::string::npos && query.find("llama.cpp") == std::string::npos)
                score -= 8;
            if (lower_rel.find("docs/") == 0 && query.find("docs") == std::string::npos && query.find("readme") == std::string::npos)
                score -= 4;
            if (lower_rel.find("cmakelists.txt") != std::string::npos)
                score -= 6;
            if (path.extension() == ".h" || path.extension() == ".hpp" || path.extension() == ".hh")
                score -= 1;
            if (path.extension() == ".cc" || path.extension() == ".cpp" || path.extension() == ".c")
                score += 2;
            if (lower_line.find("::") != std::string::npos || lower_line.find("void ") != std::string::npos || lower_line.find("class ") != std::string::npos)
                score += 1;

            if (score <= 0)
                continue;
            matches.push_back({rel, lineno, score, line, extract_symbol_from_line(line)});
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
    {
        oss << "- file=" << m.file << ":" << m.line << " score=" << m.score;
        if (!m.symbol.empty())
            oss << " symbol=" << m.symbol;
        oss << " text=" << m.text << "\n";
    }
    return truncate_text(oss.str(), options.max_tool_output_chars);
}
} // namespace

void RegisterBuiltinTools(ToolRegistry &registry,
                          const BuiltinToolsOptions &options,
                          std::function<std::string()> status_provider)
{
    if (options.search_kb_handler)
    {
        registry.Register("search_kb", [options](const nlohmann::json &input)
                          { return truncate_text(options.search_kb_handler(input), options.max_tool_output_chars); });
    }

    if (options.open_chunk_handler)
    {
        registry.Register("open_chunk", [options](const nlohmann::json &input)
                          { return truncate_text(options.open_chunk_handler(input), options.max_tool_output_chars); });
    }

    registry.Register("search_docs", [options](const nlohmann::json &input)
                      { return search_docs_tool(options, input); });

    registry.Register("search_web", [options](const nlohmann::json &input)
                      { return search_web_tool(options, input); });

    registry.Register("fetch_url", [options](const nlohmann::json &input)
                      { return fetch_url_tool(options, input); });

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
