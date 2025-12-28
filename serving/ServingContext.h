#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    bool is_finished = false;
    FinishReason finish_reason = FinishReason::stop;
};

struct ServingContext
{
    // ===== Request =====
    std::string request_id;
    std::string model;
    bool stream = false;
    std::vector<Message> messages;

    // ===== Runtime =====
    std::atomic<bool> cancelled{false};

    // SSE callback (called by engine)
    std::function<void(const StreamChunk &)> on_chunk;

    // ===== Result (non-stream) =====
    std::string final_text;
    FinishReason finish_reason = FinishReason::stop;
};
