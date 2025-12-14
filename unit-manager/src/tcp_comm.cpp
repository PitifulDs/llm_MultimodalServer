
#include <unordered_map>
#include <unistd.h>
#include <chrono>
#include <any>
#include <cstring>
#include <ctime>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "all.h"
#include "session.h"
#include "zmq_bus.h"
#include <boost/any.hpp>
#include "json.hpp"
#include <variant>

std::atomic<int> counter_port(8000);
network::EventLoop loop;
std::unique_ptr<network::TcpServer> server;
std::mutex context_mutex;

// 当客户端-服务端进行一个连接的时候，会触发这个回调函数
void onConnection(const network::TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        // 会话窗口：存在历史上下文信息，类似微信聊天窗口
        std::shared_ptr<TcpSession> session = std::make_shared<TcpSession>(conn);
        conn->setContext(session);
        // 建立连接后，做一些初始化的操作
        session->work(zmq_s_format, counter_port.fetch_add(1));

        if (counter_port > 65535)
        {
            counter_port.store(8000);
        }
    }
    else
    {
        try
        {
            auto session = boost::any_cast<std::shared_ptr<TcpSession>>(conn->getContext());
            session->stop();
        }
        catch (const std::bad_any_cast &e)
        {
            std::cerr << "Bad any_cast: " << e.what() << std::endl;
        }
    }
}

// void onConnection(const TcpConnectionPtr &conn)
// {
//     if (conn->connected())
//     {
//         Context context;
//         conn->setContext(context);
//     }
//     else
//     {
//         const Context &context = boost::any_cast<Context>(conn->getContext());
//         LOG_INFO << "payload bytes " << context.bytes;
//         conn->getLoop()->quit();
//     }
// }
void onMessage(const network::TcpConnectionPtr &conn, network::Buffer *buf)
{
    std::string msg(buf->retrieveAllAsString());

    try
    {
        // 获取会话窗口
        auto session = boost::any_cast<std::shared_ptr<TcpSession>>(conn->getContext());

        session->select_json_str(msg, std::bind(&TcpSession::on_data, session, std::placeholders::_1));
    }
    catch (const boost::bad_any_cast &e)
    {
        std::cerr << "Type cast error: " << e.what() << std::endl;
    }
}

void tcp_work()
{
    int listenport = 0;
    // 主线程，Reactor是监听
    // 获取系统的sql键值数据库中参数配置
    SAFE_READING(listenport, int, "config_tcp_server");
    network::InetAddress listenAddr(listenport);
    server = std::make_unique<network::TcpServer>(&loop, listenAddr, "ZMQBridge");

    server->setConnectionCallback(onConnection);
    server->setMessageCallback(onMessage);
    // 2个IO线程，负责处理消息
    server->setThreadNum(2);

    server->start();
    loop.loop();
}

void tcp_stop_work()
{
    loop.quit();
    server.reset();
}
