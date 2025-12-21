#pragma once

#include "http_types.h"

#include "network/TcpConnection.h"
#include "network/Buffer.h"

#include <string>
#include <unordered_map>

//
// Network → HTTP 的适配实现
//

struct NetworkHttpRequest : public HttpRequest
{
    std::unordered_map<std::string, std::string> query;

    bool HasQuery(const std::string &key) const override
    {
        return query.find(key) != query.end();
    }

    std::string Query(const std::string &key) const override
    {
        auto it = query.find(key);
        return it == query.end() ? "" : it->second;
    }
};

struct NetworkHttpResponse : public HttpResponse
{
    network::TcpConnectionPtr conn;
    bool header_sent{false};
    bool sse{false};

    explicit NetworkHttpResponse(const network::TcpConnectionPtr &c, bool stream = false)
        : conn(c), sse(stream) {}

    void SetHeader(const std::string & /*k*/,
                   const std::string & /*v*/) override
    {
        // 简化：暂不处理 header
    }

    void Write(const std::string &data) override
    {
        if (!conn)
            return;

        network::Buffer buf;

        if (!header_sent)
        {
            if (sse)
            {
                buf.append(
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\n\r\n");
            }
            else
            {
                buf.append(
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Connection: close\r\n\r\n");
            }
            header_sent = true;
        }

        buf.append(data);
        conn->send(&buf);

        conn->shutdown();
    }

    bool IsAlive() const override
    {
        return conn && conn->connected();
    }
};
