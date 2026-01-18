#pragma once

#include "http_types.h"

#include "network/TcpConnection.h"
#include "network/EventLoop.h"
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

struct NetworkHttpResponse : public HttpResponse, public std::enable_shared_from_this<NetworkHttpResponse>
{
    network::TcpConnectionPtr conn;
    bool header_sent{false};
    bool sse{false};
    std::mutex mu;

    int status_code{200};
    std::string reason{"OK"};
    std::unordered_map<std::string, std::string> headers;

    explicit NetworkHttpResponse(const network::TcpConnectionPtr &c, bool stream = false)
        : conn(c), sse(stream) 
    {}

    void SetStatus(int code, const std::string &r = "") override
    {
        if (header_sent)
            return;
        status_code = code;
        if (!r.empty())
            reason = r;
        else{
            if (code == 200)
                reason = "OK";
            else if (code == 429)
                reason = "Too Many Requests";
            else if (code == 503)
                reason = "Service Unavailable";
            else if (code == 400)
                reason = "Bad Request";
            else if (code == 500)
                reason = "Internal Server Error";
            else
                reason = "Error";
        }
    }

    void SetHeader(const std::string &k, const std::string &v) override
    {
        if (header_sent)
            return;
        headers[k] = v;
    }

    void Write(const std::string &data) override
    {
        if (!conn)
            return;

        // 关键：切回 IO 线程
        auto loop = conn->getLoop(); 
        if (loop && !loop->isInLoopThread())
        {
            auto self = shared_from_this();
            std::string copy = data; // 必须拷贝
            loop->queueInLoop([self, copy = std::move(copy)]() mutable
                              { self->WriteInLoop(copy); });
            return;
        }

        // 已在 IO 线程
        WriteInLoop(data);
    }

     void WriteInLoop(const std::string &data)
    {
        // 断言：抓问题
        if (auto loop = conn->getLoop()) {
            loop->assertInLoopThread();
        }

        if (!conn || !conn->connected()) {
            return;
        }

        // 如果担心 header_sent 在极端情况下被并发访问，可以加锁
        // std::lock_guard<std::mutex> lk(mu);

        network::Buffer buf;

        if (!header_sent)
        {
            if (sse)
            {
                buf.append("HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n");
            }
            else
            {
                buf.append("HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n");
            }
            header_sent = true;
        }

        buf.append(data);
        conn->send(&buf);

        if (!sse)
        {
            conn->shutdown();
        }
    }

    bool IsAlive() const override
    {
        return conn && conn->connected();
    }
};
