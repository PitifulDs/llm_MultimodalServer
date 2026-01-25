#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

#include "glog/logging.h"
struct Session;
class ModelEngine;

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

    // ===== Usage (OpenAI-compatible) =====
    struct Usage
    {
        int prompt_tokens = 0;
        int completion_tokens = 0;
        int total_tokens = 0;
    };

    Usage usage;

    std::shared_ptr<ModelEngine> engine;

    // ===== Finish Wait (non-stream) =====
    mutable std::mutex finish_mu;
    std::condition_variable finish_cv;
    void EmitDelta(const std::string& text)
    {
        if(finished.load()){
            return;
        }

        final_text += text;

        if(stream && on_chunk){
            StreamChunk c;
            c.delta = text;
            c.is_finished = false;
            on_chunk(c);
        }

    }

    void EmitFinish(FinishReason reason)
    {
        // 只触发一次
        if (finished.exchange(true, std::memory_order_acq_rel))
            return;

        // 先写结果（确保 WaitFinish 醒来后读到）
        finish_reason = reason;

        // 唤醒所有等待者（non-stream）
        {
            std::lock_guard<std::mutex> lk(finish_mu);
            // 只用于和 cv 配合建立 happens-before，不需要写别的
        }
        finish_cv.notify_all();

        // stream：发最后一个 chunk
        if (stream && on_chunk)
        {
            StreamChunk c;
            c.is_finished = true;
            c.finish_reason = reason;
            on_chunk(c);
        }

        // 最后：回调（可能做 history 更新等）
        if (on_finish)
            on_finish(reason);
    }

    // 等待完成（non-stream 用）
    void WaitFinish()
    {
        if (finished.load(std::memory_order_acquire))
            return;

        std::unique_lock<std::mutex> lk(finish_mu);
        finish_cv.wait(lk, [&]
                       { return finished.load(std::memory_order_acquire); });
    }

    template <class PredAlive>
    void WaitFinishOrCancel(PredAlive is_alive,  std::chrono::milliseconds poll = std::chrono::milliseconds(100))
    {
        while (!finished.load(std::memory_order_acquire))
        {
            // 连接断开：触发取消并结束
            if (!is_alive())
            {
                cancelled.store(true, std::memory_order_release);
                EmitFinish(FinishReason::cancelled); // 一次性，安全
                return;
            }

            // 等一小段时间或被 finish 唤醒
            std::unique_lock<std::mutex> lk(finish_mu);
            finish_cv.wait_for(lk, poll, [&]
                               { return finished.load(std::memory_order_acquire); });
        }
    }
};



