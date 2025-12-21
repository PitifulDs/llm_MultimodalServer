#include <glog/logging.h>
#include "HttpGateway.h"
#include "http_types.h"
#include "HttpStreamSession.h"
#include "StackFlowsClient.h"
#include "protocol/Protocol.h"

#include "../../utils/json.hpp"
#include <random>
#include <ctime>
#include <string>
#include <memory>
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

void HttpGateway::HandleCompletion(const HttpRequest &req, HttpResponse &res)
{
    // 1) 解析 JSON
    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (...)
    {
        // res.SetHeader("Content-Type", "application/json");
        // res.Write(R"({"error":"invalid json"})");
        res.Write(R"({"error":{"message":"invalid json","type":"invalid_request_error"}})");
        return;
    }

    const std::string prompt = body.value("prompt", "");
    const std::string model = body.value("model", "unkown");
    // const std::string session_id = body.value("session_id", "");

    // 2) 构造 RPC 请求（非流式）
    RpcRequest rpc;
    rpc.version = "v1";
    rpc.request_id = gen_request_id();
    rpc.session_id = body.value("session_id", "");
    rpc.action = "completion";
    rpc.stream = false;
    rpc.payload["prompt"] = prompt;

    // 3) 调用 StackFlows
    RpcResponse resp = sf_client_->Call(rpc);

    // 4) 构造openai-style 回复
    json out;
    out["id"] = "cmpl-" + rpc.request_id;
    out["object"] = "text_completion";
    out["created"] = static_cast<int>(std::time(nullptr));
    out["model"] = model;

    json choices;
    choices["index"] = 0;

    std::string text;
    auto it = resp.result.find("text");
    if (it != resp.result.end())
    {
        text = it->second;
    }
    else
    {
        text.clear();
    }

    choices["text"] = text;
    choices["finish_reason"] = "stop";

    out["choices"] = json::array({choices});
   
    res.SetHeader("Content-Type", "application/json");
    res.Write(out.dump());
}

void HttpGateway::HandleCompletionStream(const HttpRequest &req,
                                         std::shared_ptr<HttpResponse> res_ptr)

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
        res_ptr->Write(R"({"error":"invalid json"})");
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
        res_ptr->SetHeader("Content-Type", "application/json");
        res_ptr->Write(R"({"error":"stream not accepted"})");
        return;
    }

    const std::string topic = ack.stream_topic;

    // 4) 建立 SSE 会话
    // HttpStreamSession session(rpc.request_id, res);
    // session.Start();
    // 更换为智能指针，放到堆上并保持引用
    auto session = std::make_shared<HttpStreamSession>(rpc.request_id, *res_ptr);
    session->Start();
    LOG(INFO) << "[stream] sse session started";

    // 5) 订阅流式事件，捕获 shared_ptr 保证存活
    sf_client_->Subscribe(topic, [session, res_ptr, this, topic](const ZmqEvent &evt)
                          {
    LOG(INFO) << "[stream] recv event type=" << evt.type;
    session->OnEvent(evt);
    if (!session->IsAlive() || evt.type == "done" || evt.type == "error") {
        LOG(INFO) << "[stream] stream finished, unsubscribe";
        sf_client_->Unsubscribe(topic);
    } });

    // 注意：
    // - 不要在这里 return JSON
    // - HTTP 连接保持，由 SSE 持续写
}
