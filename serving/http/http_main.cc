#include "network/EventLoop.h"
#include "network/InetAddress.h"

#include "NetworkHttpServer.h"
#include "HttpGateway.h"
#include "StackFlowsClient.h"

// #include "engine/DummyEngine.h"
#include "engine/RpcEngine.h"
#include "engine/EngineFactory.h"

#include <cstdlib>
#include <memory>
#include <iostream>
/*
    1.只负责启动
    2.不写 handler 逻辑
    3.不包含业务代码
*/

int main(int argc, char **argv)
{
    // HTTP Server 初始化（后续实现）
    // StackFlowsClient 初始化
    // 路由注册
    uint16_t port = 8080;
    if (const char *env_port = std::getenv("HTTP_PORT"))
    {
        port = static_cast<uint16_t>(std::stoi(env_port));
    }
    if (argc > 1)
    {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    network::EventLoop loop;

    // 模型 warmup
    std::cout << "[serving-http] warming up model..." << std::endl;
    EngineFactory::Create("llama");
    std::cout << "[serving-http] warmup done" << std::endl;

    HttpGateway gateway;

    network::InetAddress listen_addr(port);
    NetworkHttpServer server(&loop, listen_addr, &gateway);

    std::cout << "[serving-http] listen on port " << port << std::endl;

    server.Start();
    loop.loop();
    return 0;
}
