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
    // ===== Request Identity =====
    std::string request_id;
    std::string session_id;
    std::string model;

    // ===== Request Type =====
    bool is_chat = false;
    bool stream = false;

    // ChatCompletion
    std::vector<Message> messages;

    // Completion
    std::string prompt;

    // ===== Generation Params (extensible) =====
    std::unordered_map<std::string, std::string> params;

    // ===== Runtime Control =====
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};

    // ===== Streaming Callback =====
    std::function<void(const StreamChunk &)> on_chunk;

    // ===== Result (non-stream) =====
    std::string final_text;
    FinishReason finish_reason = FinishReason::stop;
    std::string error_message;
};
