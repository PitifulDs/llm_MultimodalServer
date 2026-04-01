#include "NetworkHttpTypes.h"
#include "NetworkHttpServer.h"
#include "HttpGateway.h"
#include "http_types.h"

#include "network/TcpServer.h"
#include "network/EventLoop.h"
#include "network/Buffer.h"

#include <sstream>
#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <boost/any.hpp>
#include <glog/logging.h>
#include "utils/json.hpp"

using namespace network;
using json = nlohmann::json;

struct ParsedRequestHead
{
    bool valid_request_line{false};
    std::string method;
    std::string url;
    std::string version;

    bool has_content_length{false};
    bool invalid_content_length{false};
    size_t content_length{0};
    bool has_chunked_transfer_encoding{false};
};

std::string trim_copy(const std::string &s)
{
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;

    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;

    return s.substr(begin, end - begin);
}

std::string to_lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_size_t_strict(const std::string &s, size_t &out)
{
    if (s.empty())
        return false;
    size_t value = 0;
    for (char ch : s)
    {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return false;
        const size_t digit = static_cast<size_t>(ch - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

ParsedRequestHead parse_request_head(const std::string &header)
{
    ParsedRequestHead out;

    std::istringstream iss(header);
    std::string line;
    if (!std::getline(iss, line))
        return out;

    if (!line.empty() && line.back() == '\r')
        line.pop_back();

    {
        std::istringstream first(line);
        first >> out.method >> out.url >> out.version;
        out.valid_request_line = !out.method.empty() && !out.url.empty() && !out.version.empty();
    }

    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        const size_t sep = line.find(':');
        if (sep == std::string::npos)
            continue;

        const std::string key = to_lower_copy(trim_copy(line.substr(0, sep)));
        const std::string value = trim_copy(line.substr(sep + 1));

        if (key == "content-length")
        {
            size_t len = 0;
            if (out.has_content_length || !parse_size_t_strict(value, len))
            {
                out.invalid_content_length = true;
                continue;
            }
            out.has_content_length = true;
            out.content_length = len;
            continue;
        }

        if (key == "transfer-encoding")
        {
            const std::string lower_val = to_lower_copy(value);
            if (lower_val.find("chunked") != std::string::npos)
                out.has_chunked_transfer_encoding = true;
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_query_map(const std::string &query)
{
    std::unordered_map<std::string, std::string> out;
    size_t start = 0;
    while (start <= query.size())
    {
        const size_t amp = query.find('&', start);
        const std::string part = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!part.empty())
        {
            const size_t eq = part.find('=');
            if (eq == std::string::npos)
                out[part] = "";
            else
                out[part.substr(0, eq)] = part.substr(eq + 1);
        }

        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }
    return out;
}

std::optional<bool> parse_query_stream_value(const std::unordered_map<std::string, std::string> &query)
{
    auto it = query.find("stream");
    if (it == query.end())
        return std::nullopt;

    const std::string value = to_lower_copy(trim_copy(it->second));
    if (value == "true" || value == "1")
        return true;
    if (value == "false" || value == "0")
        return false;
    return std::nullopt;
}

std::optional<bool> parse_body_stream_hint(const std::string &body)
{
    if (body.empty())
        return std::nullopt;
    try
    {
        const json parsed = json::parse(body);
        if (parsed.contains("stream") && parsed["stream"].is_boolean())
            return parsed["stream"].get<bool>();
    }
    catch (...)
    {
    }
    return std::nullopt;
}

static void write_json_error(const std::shared_ptr<NetworkHttpResponse> &res_ptr,
                             int status,
                             const std::string &message,
                             const std::string &type,
                             const std::string &code = "")
{
    res_ptr->SetStatus(status);
    res_ptr->SetHeader("Content-Type", "application/json");
    res_ptr->SetHeader("Connection", "close");

    json err = {{"error", {{"message", message}, {"type", type}}}};
    if (!code.empty())
        err["error"]["code"] = code;
    res_ptr->Write(err.dump(-1, ' ', false, json::error_handler_t::replace));
    res_ptr->End();
}
NetworkHttpServer::NetworkHttpServer(EventLoop *loop,
                                     const InetAddress &listen_addr,
                                     HttpGateway *gateway)
    : server_(loop, listen_addr, "HttpServer"),
      gateway_(gateway)
{
    server_.setConnectionCallback(
        std::bind(&NetworkHttpServer::onConnection, this, std::placeholders::_1));

    server_.setMessageCallback(
        std::bind(&NetworkHttpServer::onMessage, this,
                  std::placeholders::_1,
                  std::placeholders::_2));
}

void NetworkHttpServer::Start()
{
    server_.start();
}

void NetworkHttpServer::onConnection(const TcpConnectionPtr &conn)
{
    if (!conn->connected())
    {
        http_buffers_.erase(conn);
        const auto &ctx = conn->getContext();
        if (!ctx.empty())
        {
            auto cb = boost::any_cast<std::function<void()>>(&ctx);
            if (cb && *cb)
            {
                (*cb)();
            }
        }
    }
}

void NetworkHttpServer::onMessage(
    const TcpConnectionPtr &conn,
    network::Buffer *buf)
{
    std::string &cache = http_buffers_[conn];
    cache.append(buf->retrieveAllAsString());

    handleHttpRequest(conn, cache);
}

void NetworkHttpServer::handleHttpRequest(
    const TcpConnectionPtr &conn,
    std::string &buffer)
{
    const auto pos = buffer.find("\r\n\r\n");
    if (pos == std::string::npos)
        return;

    const std::string header = buffer.substr(0, pos);
    const ParsedRequestHead head = parse_request_head(header);

    if (!head.valid_request_line)
    {
        auto res_ptr = std::make_shared<NetworkHttpResponse>(conn, false);
        write_json_error(res_ptr, 400, "invalid request line", "invalid_request_error", "invalid_request_line");
        return;
    }

    if (head.has_chunked_transfer_encoding)
    {
        auto res_ptr = std::make_shared<NetworkHttpResponse>(conn, false);
        write_json_error(res_ptr,
                         400,
                         "transfer-encoding: chunked is not supported; please send content-length",
                         "invalid_request_error",
                         "unsupported_transfer_encoding");
        return;
    }

    if (head.invalid_content_length)
    {
        auto res_ptr = std::make_shared<NetworkHttpResponse>(conn, false);
        write_json_error(res_ptr, 400, "invalid content-length", "invalid_request_error", "invalid_content_length");
        return;
    }

    if (head.method == "POST" && !head.has_content_length)
    {
        auto res_ptr = std::make_shared<NetworkHttpResponse>(conn, false);
        write_json_error(res_ptr, 411, "content-length required", "invalid_request_error", "length_required");
        return;
    }

    LOG(INFO) << "[http] header_len=" << header.size()
              << ", content_length=" << head.content_length
              << ", buffer_size=" << buffer.size();
    LOG(INFO) << "[http] raw header >>>" << header << "<<<";

    const size_t content_length = head.has_content_length ? head.content_length : 0;
    const size_t total_len = pos + 4 + content_length;
    if (buffer.size() < total_len)
        return; // body 还没收全

    const std::string body = buffer.substr(pos + 4, content_length);

    // 🔥 消费掉已处理的数据
    buffer.erase(0, total_len);

    LOG(INFO) << "[http] body_len=" << body.size() << " raw body >>>" << body << "<<<";

    std::string method = head.method;
    std::string url = head.url;

    // 3. 构造 Request
    NetworkHttpRequest req;
    req.body = body;

    // 4. 解析 query
    auto qpos = url.find('?');
    if (qpos != std::string::npos)
    {
        const std::string query = url.substr(qpos + 1);
        req.query = parse_query_map(query);
        url = url.substr(0, qpos);
    }

    // stream 判定：body.stream 为准，query.stream 仅兼容兜底
    const std::optional<bool> query_stream = parse_query_stream_value(req.query);
    const std::optional<bool> body_stream =
        (method == "POST" && (url == "/v1/chat/completions" || url == "/v1/completions"))
            ? parse_body_stream_hint(body)
            : std::nullopt;
    const bool is_stream = body_stream.has_value() ? *body_stream : query_stream.value_or(false);

    // 5. Response
    auto res_ptr = std::make_shared<NetworkHttpResponse>(conn, is_stream);

    // 6. 路由（先处理 CORS 预检）
    if (method == "OPTIONS")
    {
        res_ptr->SetStatus(204, "No Content");
        res_ptr->SetHeader("Access-Control-Allow-Origin", "*");
        res_ptr->SetHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
        res_ptr->SetHeader("Access-Control-Allow-Headers", "content-type");
        res_ptr->SetHeader("Access-Control-Max-Age", "86400");
        res_ptr->SetHeader("Connection", "close");
        res_ptr->Write("");
        res_ptr->End();
        return;
    }

    if (method == "GET" && url == "/health")
    {
        gateway_->HandleHealth(req, *res_ptr);
        return;
    }

    if (method == "GET" && url == "/metrics")
    {
        gateway_->HandleMetrics(req, *res_ptr);
        return;
    }

    if (method == "GET" && url == "/v1/models")
    {
        gateway_->HandleModels(req, *res_ptr);
        return;
    }

    if (method == "GET" && url == "/admin/rag/status")
    {
        gateway_->HandleAdminRagStatus(req, *res_ptr);
        return;
    }

    if (method != "POST")
    {
        write_json_error(res_ptr, 405, "Method Not Allowed", "invalid_request_error", "method_not_allowed");
        return;
    }

    if (method == "POST" && url == "/v1/completions")
    {
        if (is_stream)
            gateway_->HandleCompletionStream(req, res_ptr);
        else
            gateway_->HandleCompletion(req, *res_ptr);
    }else if(method == "POST" && url == "/v1/chat/completions")
    {
        if(is_stream){
            gateway_->HandleChatCompletionStream(req, res_ptr);
        }else{
            gateway_->HandleChatCompletion(req, *res_ptr);
        }

    }
    else if (method == "POST" && url == "/v1/retrieval/search")
    {
        gateway_->HandleRetrievalSearch(req, *res_ptr);
    }
    else if (method == "POST" && url == "/v1/agent/debug")
    {
        gateway_->HandleAgentDebug(req, *res_ptr);
    }
    else if (method == "POST" && url == "/admin/rag/reload-index")
    {
        gateway_->HandleAdminRagReloadIndex(req, *res_ptr);
    }
    else
    {
        write_json_error(res_ptr, 404, "Not Found", "invalid_request_error", "not_found");
    }
}
