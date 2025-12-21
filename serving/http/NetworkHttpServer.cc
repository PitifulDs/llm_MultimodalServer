#include "NetworkHttpTypes.h"
#include "NetworkHttpServer.h"
#include "HttpGateway.h"
#include "http_types.h"

#include "network/TcpServer.h"
#include "network/EventLoop.h"
#include "network/Buffer.h"

#include <sstream>
#include <string_view>
#include <algorithm>
#include <cctype>
#include <glog/logging.h>

using namespace network;

// 只在本 .cc 文件使用的话，建议 static
static size_t parse_content_length(const std::string &header)
{
    // 逐行扫描 header
    std::istringstream iss(header);
    std::string line;

    while (std::getline(iss, line))
    {
        // 去掉行尾 \r
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        // 转成小写做匹配（只针对前缀部分即可）
        // 允许 "Content-Length:" / "content-length:"
        const std::string key = "content-length:";
        if (line.size() >= key.size())
        {
            bool match = true;
            for (size_t i = 0; i < key.size(); ++i)
            {
                char a = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
                if (a != key[i])
                {
                    match = false;
                    break;
                }
            }

            if (match)
            {
                // 取冒号后面的数字，跳过空格
                size_t p = key.size();
                while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
                {
                    ++p;
                }

                // 解析数字
                size_t len = 0;
                while (p < line.size() && std::isdigit(static_cast<unsigned char>(line[p])))
                {
                    len = len * 10 + (line[p] - '0');
                    ++p;
                }
                return len;
            }
        }
    }

    // 没有 Content-Length：按 0 处理
    return 0;
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
        // 断开连接
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
    // 1. 拆 header
    auto pos = buffer.find("\r\n\r\n");
    if (pos == std::string::npos)
        return;

    std::string header = buffer.substr(0, pos);
    size_t content_length = parse_content_length(header);

    // 若未带 Content-Length，则退回到已有数据长度推断
    if (content_length == 0 && buffer.size() >= pos + 4)
    {
        content_length = buffer.size() - (pos + 4);
    }

    LOG(INFO) << "[http] header_len=" << header.size()
              << ", content_length=" << content_length
              << ", buffer_size=" << buffer.size();
    LOG(INFO) << "[http] raw header >>>" << header << "<<<";

    size_t total_len = pos + 4 + content_length;
    if (buffer.size() < total_len)
        return; // body 还没收全

    std::string body = buffer.substr(pos + 4, content_length);

    // 🔥 消费掉已处理的数据
    buffer.erase(0, total_len);

    LOG(INFO) << "[http] body_len=" << body.size() << " raw body >>>" << body << "<<<";

    // 2. 解析请求行
    std::istringstream iss(header);
    std::string method, url, version;
    iss >> method >> url >> version;

    // 3. 构造 Request
    NetworkHttpRequest req;
    req.body = body;

    // 4. 解析 query
    bool is_stream = false;
    auto qpos = url.find('?');
    if (qpos != std::string::npos)
    {
        std::string query = url.substr(qpos + 1);
        if (query.find("stream=true") != std::string::npos)
        {
            is_stream = true;
            req.query["stream"] = "true";
        }
        url = url.substr(0, qpos);
    }

    // 5. Response
    NetworkHttpResponse res(conn, is_stream);

    // 6. 路由
    if (method == "POST" && url == "/v1/completions")
    {
        auto res_ptr = std::make_shared<NetworkHttpResponse>(conn, is_stream);
        if (is_stream)
            gateway_->HandleCompletionStream(req, res_ptr);
        else
            gateway_->HandleCompletion(req, *res_ptr);
    }
    else
    {
        res.Write("Not Found");
    }
}
