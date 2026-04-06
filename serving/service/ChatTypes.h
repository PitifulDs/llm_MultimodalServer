#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "utils/json.hpp"
#include "serving/core/CompletionTypes.h"
#include "serving/core/ModelCapability.h"

struct ServingContext;

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

struct ChatExecutionRequest
{
    std::shared_ptr<ServingContext> ctx;
    std::vector<Message> client_messages;
};

struct ChatResponse
{
    std::string model;
    std::string output_text;
    FinishReason finish_reason = FinishReason::stop;
    std::string error_message;
    UsageInfo usage;
    nlohmann::json references = nlohmann::json();
    nlohmann::json retrieval = nlohmann::json();
    nlohmann::json agent_result = nlohmann::json();
    nlohmann::json subqueries = nlohmann::json();
    nlohmann::json evidence = nlohmann::json();
    nlohmann::json agent_trace = nlohmann::json();
};

enum class ChatErrorKind
{
    None,
    InvalidRequest,
    ServiceUnavailable,
    RateLimit,
    Internal
};

struct ChatError
{
    ChatErrorKind kind = ChatErrorKind::None;
    std::string code;
    std::string message;

    bool HasError() const
    {
        return kind != ChatErrorKind::None;
    }
};
