#include "RpcEngine.h"

#include <utility>

#include "serving/http/protocol/Protocol.h"
#include "serving/http/StackFlowsClient.h"
#include "utils/json.hpp"
#include <glog/logging.h>

using json = nlohmann::json;

RpcEngine::RpcEngine(StackFlowsClient *client)
    : sf_client_(client)
{
}


void RpcEngine::run(std::shared_ptr<ServingContext> ctx)
{
    LOG(INFO) << "[RpcEngine] run: request_id=" << ctx->request_id
              << " stream=" << ctx->stream
              << " is_chat=" << ctx->is_chat;

    // 1. 构造 RPC 请求（统一入口）
    RpcRequest rpc;
    rpc.version = "v1";
    rpc.request_id = ctx->request_id;
    rpc.session_id = ctx->session_id;
    rpc.action = ctx->is_chat ? "chat_completion" : "completion";
    rpc.stream = ctx->stream;

    // payload：由 Context 决定
    if (ctx->is_chat)
    {
        // Chat：messages 或 prompt（取你现有协议）
        json msgs = json::array();
        for(const auto &m : ctx->messages)
        {
            msgs.push_back({{"role", m.role},
                            {"content", m.content}});
        }
        rpc.payload["messages"] = msgs.dump();
    }
    else
    {
        // Completion
        rpc.payload["prompt"] = ctx->prompt;
    }

    // 2. 非流式：同步 Call
    if (!ctx->stream)
    {
        RpcResponse resp = sf_client_->Call(rpc);
        
        // 取结果
        auto it = resp.result.find("text");
        if(it != resp.result.end()){
            ctx->final_text = it->second;
        }
        
        ctx->finish_reason = FinishReason::stop;
        return;

    }

    // 3. 流式：Subscribe
    // 约定 topic（和 Phase 1 保持一致）
    std::string topic = "llm.stream." + ctx->request_id;

    // 3.1 注册订阅
    sf_client_->Subscribe(topic, [ctx](const ZmqEvent &evt)
    {
        if (!ctx->on_chunk)
        {
            return;
        }

        if (evt.type == "delta")
        {
            StreamChunk ch;
            ch.delta = evt.data;
            ch.is_finished = false;
            ctx->on_chunk(ch);
            return;
        }

        if (evt.type == "done")
        {
            StreamChunk ch;
            ch.is_finished = true;
            ch.finish_reason = FinishReason::stop;
            ctx->on_chunk(ch);
            return;
        }

        if (evt.type == "error")
        {
            StreamChunk ch;
            ch.is_finished = true;
            ch.finish_reason = FinishReason::error;
            ctx->on_chunk(ch);
            return;
        }
    });

    // 3.2 发送真正的 RPC（触发后端开始推流）
    sf_client_->Call(rpc);
}