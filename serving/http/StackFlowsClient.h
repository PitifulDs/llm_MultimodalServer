#pragma once

#include <functional>
#include <string>
#include "protocol/Protocol.h"

/**
 * @brief StackFlows 客户端接口
 *
 * HTTP Gateway 通过该接口与内部 StackFlows 通信。
 * 本接口屏蔽 ZMQ / RPC 实现细节。
 * 这是一个 非常薄的 Client
 * 屏蔽 ZMQ / RPC 细节
 * HTTP 层只认它
 */


class StackFlowsClient
{
public:
    StackFlowsClient() = default;
    ~StackFlowsClient() = default;

    // 同步 RPC 调用（非流式 or 启动流式）
    RpcResponse Call(const RpcRequest &request);

    // 订阅流式事件（PUB/SUB）
    void Subscribe(const std::string &topic,
                   std::function<void(const ZmqEvent &)> callback);

    // 取消订阅（HTTP 断连时调用）
    void Unsubscribe(const std::string &topic);
 };