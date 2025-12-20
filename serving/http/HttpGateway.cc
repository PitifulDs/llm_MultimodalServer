#include <glog/logging.h>
#include "HttpGateway.h"
#include "http_types.h"
#include "HttpStreamSession.h"
#include "StackFlowsClient.h"
#include "protocol/Protocol.h"

#include "../../utils/json.hpp"
#include <random>

using json = nlohmann::json;

namespace
{

    // 简单生成 request_id（示例）
    std::string gen_request_id()
    {
        static std::atomic<uint64_t> seq{0};
        return "req-" + std::to_string(++seq);
    }

} // namespace

HttpGateway::HttpGateway(StackFlowsClient *client)
    : sf_client_(client)
{
}

void HttpGateway::HandleCompletion(const HttpRequest &req,
                                   HttpResponse &res)
{
    // 1) 解析 JSON
    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (...)
    {
        res.SetHeader("Content-Type", "application/json");
        res.Write(R"({"error":"invalid json"})");
        return;
    }

    const std::string prompt = body.value("prompt", "");
    const std::string session_id = body.value("session_id", "");

    // 2) 构造 RPC 请求（非流式）
    RpcRequest rpc;
    rpc.version = "v1";
    rpc.request_id = gen_request_id();
    rpc.session_id = session_id;
    rpc.action = "completion";
    rpc.stream = false;
    rpc.payload["prompt"] = prompt;

    // 3) 调用 StackFlows
    RpcResponse resp = sf_client_->Call(rpc);

    res.SetHeader("Content-Type", "application/json");
    json out;
    out["status"] = resp.status;
    out["request_id"] = resp.request_id;
    if (!resp.result.empty())
    {
        out["result"] = resp.result;
    }
    res.Write(out.dump());
}

void HttpGateway::HandleCompletionStream(const HttpRequest &req,
                                         HttpResponse &res)
{
    LOG(INFO) << "[stream] enter HandleCompletionStream";
    LOG(INFO) << "[stream] raw body >>>" << req.body << "<<<";

    // 1) 解析 JSON
    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (...)
    {
        // res.SetHeader("Content-Type", "application/json");
        LOG(ERROR) << "[stream] json parse failed";
        res.Write(R"({"error":"invalid json"})");
        return;
    }

    const std::string prompt = body.value("prompt", "");
    const std::string session_id = body.value("session_id", "");
    LOG(INFO) << "[stream] prompt len=" << prompt.size();

    // 2) 构造 RPC 请求（启动流）
    RpcRequest rpc;
    rpc.version = "v1";
    rpc.request_id = gen_request_id();
    rpc.session_id = session_id;
    rpc.action = "completion";
    rpc.stream = true;
    rpc.payload["prompt"] = prompt;
    LOG(INFO) << "[stream] send rpc, req_id=" << rpc.request_id;

    // 3) 调用 StackFlows（启动流）
    RpcResponse ack = sf_client_->Call(rpc);
    LOG(INFO) << "[stream] rpc ack status=" << ack.status
              << ", topic=" << ack.stream_topic;

    if (ack.status != "accepted" || ack.stream_topic.empty())
    {
        res.SetHeader("Content-Type", "application/json");
        res.Write(R"({"error":"stream not accepted"})");
        return;
    }

    const std::string topic = ack.stream_topic;

    // 4) 建立 SSE 会话
    HttpStreamSession session(rpc.request_id, res);
    session.Start();
    LOG(INFO) << "[stream] sse session started";

    // 5) 订阅流式事件
    sf_client_->Subscribe(topic, [&](const ZmqEvent &evt)
                          {
        LOG(INFO) << "[stream] recv event type=" << evt.type;
        session.OnEvent(evt);

        // 若客户端断开或 session 结束，反订阅
        if (!session.IsAlive()) {
            LOG(INFO) << "[stream] session closed, unsubscribe";
            sf_client_->Unsubscribe(topic);
        } });

    // 注意：
    // - 不要在这里 return JSON
    // - HTTP 连接保持，由 SSE 持续写
}
