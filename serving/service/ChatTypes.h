#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "serving/core/CompletionTypes.h"
#include "serving/core/ModelCapability.h"

struct ChatRequest
{
    std::string request_id;
    std::string model;
    std::string inference_backend;
    bool stream = false;
    std::vector<Message> messages;
    std::unordered_map<std::string, std::string> params;
    std::function<void(const StreamChunk &)> on_chunk;
    std::function<void(FinishReason)> on_finish;
    ModelCapability capability = ModelCapability::Chat;
};

struct ChatResponse
{
    std::string model;
    std::string output_text;
    FinishReason finish_reason = FinishReason::stop;
    std::string error_message;
    UsageInfo usage;
};
