#include "http_types.h" 
#include "HttpStreamSession.h"
#include "protocol/Protocol.h"
#include "../../utils/json.hpp" // nlohmann::json
#include <sstream>

using json = nlohmann::json;

HttpStreamSession::HttpStreamSession(const std::string &request_id, HttpResponse& response)
                    : request_id_(request_id), response_(response)
{
}

HttpStreamSession::~HttpStreamSession()
{
    Close();
}

void HttpStreamSession::Start()
{
    // SSE 必要头
    response_.SetHeader("Content-Type", "text/event-stream");
    response_.SetHeader("Cache-Control", "no-cache");
    response_.SetHeader("Connection", "keep-alive");

    // 可选：告诉客户端我们开始了
    // write_sse(R"({"type":"open"})");
}

bool HttpStreamSession::IsAlive() const
{
    return !closed_.load();
}

void HttpStreamSession::OnEvent(const ZmqEvent &event)
{
    if (!IsAlive())
    {
        Close();
        return;
    }

    if (event.type == "delta")
    {
        // // 直接转发增量
        // write_sse(event.data);
        OnDelta(event.data);
        return;
    }

    if (event.type == "done")
    {
        // write_sse("[DONE]");
        // Close();
        OnDone();
        return;
    }

    if (event.type == "error")
    {
        // 统一错误格式（最小）
        write_sse(R"({"error":"stream error"})");
        Close();
        return;
    }

    // 其它未知类型，直接忽略或记录
}

void HttpStreamSession::Close()
{
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true))
    {
        return; // 已关闭
    }
    // 不再写数据；订阅的释放在 Gateway/Client 层做
}

void HttpStreamSession::write_sse(const std::string &data)
{
    if (!IsAlive())
        return;

    // SSE 格式：data: <payload>\n\n
    std::ostringstream oss;
    oss << "data: " << data << "\n\n";
    response_.Write(oss.str());
}

void HttpStreamSession::OnDelta(const std::string &text)
{
    if (closed_.load())
        return;

    json chunk = {
        {"id", request_id_},
        {"object", "text_completion.chunk"},
        {"created", static_cast<int>(std::time(nullptr))},
        {"model", "dummy"},
        {"choices", {{
            {"index", 0}, 
            {"delta", {{"content", text}}}, 
            {"finish_reason", nullptr}
        }}}
    };

    write_sse(chunk.dump());
}

void HttpStreamSession::OnDone()
{
    if (closed_.load())
        return;

    json chunk = {
        {"id", request_id_},
        {"object", "text_completion.chunk"},
        {"created", static_cast<int>(std::time(nullptr))},
        {"model", "dummy"},
        {"choices", {{
            {"index", 0}, 
            {"delta", json::object()}, 
            {"finish_reason", "stop"}
        }}}
    };

    write_sse(chunk.dump());
    write_sse("[DONE]");

    closed_.store(true);
}
