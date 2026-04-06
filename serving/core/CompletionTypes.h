#pragma once

#include <string>

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
    std::string metadata_json;
    bool is_finished = false;
    FinishReason finish_reason = FinishReason::stop;
};

struct UsageInfo
{
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
};
