#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "glog/logging.h"
struct Session;

enum class FinishReason
{
    stop,
    length,
    cancelled,
    error
};

struct Message
{
    std::string role;
    std::string content;
};

struct StreamChunk
{
    std::string delta;
    bool is_finished = false; // 是否为“最后一个 chunk”
    FinishReason finish_reason = FinishReason::stop;
};

struct ServingContext
{
    ServingContext() = default;
    ServingContext(const ServingContext &) = delete;            // 禁用 copy
    ServingContext &operator=(const ServingContext &) = delete; // 禁用 copy

    ServingContext(ServingContext &&) = default;                // 允许 move
    ServingContext &operator=(ServingContext &&) = default;


    // ===== Request Identity =====
    std::string request_id;
    std::string session_id;
    std::string model;

    // ===== Request Type =====
    bool is_chat = false;
    bool stream = false;
    // 首轮标记（Engine 内设置，只读给下游）
    bool is_first_turn = false;

    // ChatCompletion
    std::vector<Message> messages;
    
    // ===== Session =====
    std::shared_ptr<Session> session; //

    // Completion
    std::string prompt;

    // ===== Generation Params (extensible) =====
    std::unordered_map<std::string, std::string> params;

    // ===== Runtime Control =====
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};

    // ===== Streaming Callback =====
    std::function<void(const StreamChunk &)> on_chunk;

    // ===== Finish Callback (always) =====
    std::function<void(FinishReason)> on_finish;

    // ===== Result (non-stream) =====
    std::string final_text;
    FinishReason finish_reason = FinishReason::stop;
    std::string error_message;

    void EmitDelta(const std::string& text)
    {
        StreamChunk c;
        c.delta = text;
        c.is_finished = false;

        if(stream){
            if (on_chunk)
            {
                on_chunk(c);
            }
                
        }else{
            final_text += text;
        }
    }

    void EmitFinish(FinishReason reason)
    { 
        StreamChunk c;
        c.is_finished = true;
        c.finish_reason = reason;

        if(stream){
            if (on_chunk)
            {
                on_chunk(c);
            }    
        }else{
            finish_reason = reason;
        }

        // 无论 stream/non-stream，都触发 finish
        LOG(INFO) << "[ctx] before on_finish req=" << request_id;
        if (on_finish)
            on_finish(reason);
        LOG(INFO) << "[ctx] after  on_finish req=" << request_id;

        LOG(INFO) << "[ctx] EmitFinish req=" << request_id
                  << " reason=" << static_cast<int>(reason)
                  << " stream=" << stream;
    }
};



