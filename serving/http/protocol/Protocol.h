#pragma once

#include <map>
#include <string>

/**
 * @brief HTTP Gateway → StackFlows 的 RPC 请求描述
 */
struct RpcRequest
{
    std::string version;                        // 协议版本，如 "v1"
    std::string request_id;                     // 唯一请求 ID
    std::string session_id;                     // 会话 ID（可选）
    std::string action;                         // completion / session.reset / ...
    bool stream = false;                        // 是否流式
    std::map<std::string, std::string> payload; // 参数（扁平化）
};

/**
 * @brief StackFlows → HTTP Gateway 的 RPC 响应
 */
struct RpcResponse
{
    std::string request_id;
    std::string status;                        // ok / accepted / error
    std::map<std::string, std::string> result; // 结果
    std::string stream_topic;                  // 流式时返回的 PUB/SUB topic
};

/**
 * @brief StackFlows → HTTP Gateway 的流式事件
 */
struct ZmqEvent
{
    std::string request_id;
    std::string type; // delta / done / error
    std::string data; // token / 文本片段 / 错误信息
};
