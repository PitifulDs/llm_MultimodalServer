#pragma once

#include <string>
#include <atomic>
#include "protocol/Protocol.h"

// 前向声明
struct ZmqEvent;
class HttpResponse;
enum class StreamMode { 
    Completion, Chat 
};



/**
 * @brief HTTP 流式会话
 *
 * 表示一个 HTTP SSE 连接，对应一个 ZMQ PUB/SUB 订阅。
 *  1.生命周期 = 一个HTTP 连接
 *  2.只负责流式转发，把内部流式事件转成 SSE 输出
 *  3.不理解业务含义
 */
class HttpStreamSession
{
public:
    HttpStreamSession(const std::string &request_id, HttpResponse &response, 
                        StreamMode mode, const std::string &model);
    ~HttpStreamSession();

    // 初始化 SSE 响应
    void Start();

    // 处理来自 ZMQ 的流式事件
    void OnEvent(const ZmqEvent &event);

    // 主动关闭（DONE / ERROR / 客户端断开）
    void Close();

    // 查询是否仍可写
    bool IsAlive() const;

    // OpenAI Streaming JSON
    void OnDelta(const std::string &text);
    void OnDone();

private: 
    void write_sse(const std::string &data);

private:
    std::string request_id_;
    HttpResponse &response_;
    StreamMode mode_;
    std::string model_;
    bool sent_role_{false};
    std::atomic<bool> closed_{false};
};
