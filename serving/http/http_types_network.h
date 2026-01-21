// http_types_network.h
#include "http_types.h"
#include "network/TcpConnection.h"

#include <functional>

struct NetworkHttpResponse : public HttpResponse
{
    network::TcpConnectionPtr conn;
    bool header_sent{false};
    std::function<void()> on_close_;

    explicit NetworkHttpResponse(const network::TcpConnectionPtr &c)
        : conn(c) {}

    void SetHeader(const std::string &, const std::string &) override {}

    void SetStatus(int /*code*/, const std::string & /*reason*/ = "") override {}

    void End() override
    {
        if (conn)
            conn->shutdown();
    }

    void SetOnClose(std::function<void()> cb) override
    {
        on_close_ = std::move(cb);
        if (conn)
        {
            conn->setContext(on_close_);
        }
    }

    void Write(const std::string &data) override
    {
        if (!conn)
            return;

        network::Buffer buf;

        if (!header_sent)
        {
            buf.append(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Connection: close\r\n"
                "\r\n");
            header_sent = true;
        }

        buf.append(data);
        conn->send(&buf);
    }

    bool IsAlive() const override
    {
        return conn && conn->connected();
    }
};
