#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "network/TcpServer.h"
#include "network/TcpConnection.h"
#include "network/EventLoop.h"
#include "network/InetAddress.h"
#include "network/Buffer.h"
#include "network/util.h"


class HttpGateway;
struct HttpRequest;
struct HttpResponse;

/**
 * @brief 基于 network::TcpServer 的最小 HTTP Server
 *
 * 只负责：
 * - HTTP 解析
 * - HttpRequest / HttpResponse 适配
 * - 调用 HttpGateway
 */
class NetworkHttpServer
{
public:
    NetworkHttpServer(network::EventLoop *loop,
                      const network::InetAddress &listen_addr,
                      HttpGateway *gateway);

    void Start();

private:
    void onConnection(const network::TcpConnectionPtr &conn);
    void onMessage(const network::TcpConnectionPtr &conn,
                   network::Buffer *buf);

    void handleHttpRequest(const network::TcpConnectionPtr &conn,
                           std::string &buffer);

private:
    network::TcpServer server_;
    HttpGateway *gateway_;
    std::unordered_map<network::TcpConnectionPtr, std::string> http_buffers_;
};
