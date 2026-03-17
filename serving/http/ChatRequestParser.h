#pragma once

#include <memory>
#include <string>
#include <vector>

#include "serving/core/ServingContext.h"

class SessionManager;

struct ParsedChatRequest
{
    std::shared_ptr<ServingContext> ctx;
    std::vector<Message> client_messages;
};

struct ChatRequestParseResult
{
    bool ok = false;
    int status = 500;
    std::string message;
    std::string type;
    std::string code;
    ParsedChatRequest request;
};

ChatRequestParseResult ParseChatRequestBody(const std::string &body_text,
                                           bool stream,
                                           SessionManager &session_mgr,
                                           const std::string &default_model,
                                           int default_max_tokens,
                                           const std::string &request_id);
