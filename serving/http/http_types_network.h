// http_types_network.h
#include "http_types.h"
#include "network/TcpConnection.h"

struct NetworkHttpResponse : public HttpResponse
{
    network::TcpConnectionPtr conn;
    bool header_sent{false};

    explicit NetworkHttpResponse(const network::TcpConnectionPtr &c)
        : conn(c) {}

    void SetHeader(const std::string &, const std::string &) override {}

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
