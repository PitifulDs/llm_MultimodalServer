#include "StackFlowsClient.h"
#include "protocol/Protocol.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cstdint>

namespace {
    // 用于模拟流式推送的线程控制
struct 
StreamWorker

{
    std::atomic<bool> running{true};
    std::thread th;
};

std::unordered_map<std::string, std::shared_ptr<StreamWorker>> g_stream_workers;
std::mutex g_mutex;
}

RpcResponse StackFlowsClient::Call(const RpcRequest &request)
{ 
    RpcResponse resp;
    resp.request_id = request.request_id;

    // 非流式 completion
    if(!request.stream){
        resp.status = "ok";
        resp.result["text"] = "【stub】这是来自 StackFlowsClient 的模拟回复";
        return resp;
    }

    // 流式 completion：返回 accepted + topic
    resp.status = "accepted";
    resp.stream_topic = "stream." + request.request_id;
    return resp;
}

void StackFlowsClient::Subscribe(
    const std::string &topic,
    std::function<void(const ZmqEvent &)> callback)
{

    auto worker = std::make_shared<StreamWorker>();

    worker->th = std::thread([topic, callback, worker]()
                             {
        const char* tokens[] = {
            "Hello", " ", "from", " ", "stub", " ", "stream", "!"
        };

        for (auto tk : tokens) {
            if (!worker->running.load()) return;

            ZmqEvent evt;
            evt.type = "delta";
            evt.data = tk;
            callback(evt);

            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        if (!worker->running.load()) return;

        ZmqEvent done;
        done.type = "done";
        callback(done); });

    std::lock_guard<std::mutex> lk(g_mutex);
    g_stream_workers[topic] = worker;
}

void StackFlowsClient::Unsubscribe(const std::string &topic)
{
    std::lock_guard<std::mutex> lk(g_mutex);

    auto it = g_stream_workers.find(topic);
    if (it == g_stream_workers.end())
    {
        return;
    }

    it->second->running = false;
    if (it->second->th.joinable())
    {
        if (it->second->th.get_id() == std::this_thread::get_id())
        {
            it->second->th.detach(); // 或者延迟回收
        }
        else
        {
            it->second->th.join();
        }
    }
    g_stream_workers.erase(it);
}
