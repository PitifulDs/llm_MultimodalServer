#include "serving/http/ChatRequestParser.h"

#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
#include "serving/core/SessionManager.h"
#include "serving/http/HttpUtils.h"
#include "utils/json.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace
{
bool get_agent_enabled(const json &body)
{
    return body.contains("agent") && body["agent"].is_boolean() && body["agent"].get<bool>();
}

int get_agent_max_steps(const json &body)
{
    if (!body.contains("max_steps") || !body["max_steps"].is_number_integer())
        return 0;

    const int v = body["max_steps"].get<int>();
    if (v <= 0)
        return 0;
    return std::min(v, 8);
}

std::vector<std::string> get_agent_tools(const json &body)
{
    std::vector<std::string> tools;
    if (!body.contains("tools") || !body["tools"].is_array())
        return tools;

    for (const auto &item : body["tools"])
    {
        if (item.is_string())
            tools.push_back(item.get<std::string>());
    }
    return tools;
}

bool is_prefix(const std::vector<Message> &history,
               const std::vector<Message> &incoming)
{
    return http_utils::is_prefix(history, incoming);
}

std::vector<Message> diff_messages(const std::vector<Message> &history,
                                   const std::vector<Message> &incoming)
{
    return http_utils::diff_messages(history, incoming);
}
} // namespace

ChatRequestParseResult ParseChatRequestBody(const std::string &body_text,
                                           bool stream,
                                           SessionManager &session_mgr,
                                           const std::string &default_model,
                                           int default_max_tokens,
                                           const std::string &request_id)
{
    ChatRequestParseResult result;
    result.status = 400;
    result.type = "invalid_request_error";

    json body;
    try
    {
        body = json::parse(body_text);
    }
    catch (...)
    {
        result.message = "invalid json";
        result.code = "invalid_json";
        return result;
    }

    if (!body.contains("messages") || !body["messages"].is_array())
    {
        result.message = "messages must be array";
        result.code = "invalid_messages";
        return result;
    }

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = request_id;
    ctx->model = body.value("model", default_model);
    ctx->stream = stream;
    ctx->is_chat = true;
    ctx->use_agent = get_agent_enabled(body);
    ctx->agent_max_steps = get_agent_max_steps(body);
    ctx->agent_tools = get_agent_tools(body);

    if (body.contains("session_id") && body["session_id"].is_string())
        ctx->session_id = body["session_id"].get<std::string>();
    else
        ctx->session_id = request_id;

    ctx->session = session_mgr.getOrCreate(ctx->session_id, ctx->model);

    if (body.contains("max_tokens") && body["max_tokens"].is_number_integer())
    {
        const int max_tokens = body["max_tokens"].get<int>();
        if (max_tokens > 0)
            ctx->params["max_tokens"] = std::to_string(max_tokens);
    }
    else if (default_max_tokens > 0)
    {
        ctx->params["max_tokens"] = std::to_string(default_max_tokens);
    }

    for (const auto &m : body["messages"])
    {
        ctx->messages.push_back({m.value("role", ""), m.value("content", "")});
    }
    http_utils::set_sampling_params(body, ctx->params);

    result.request.client_messages = ctx->messages;

    auto session = ctx->session;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        const std::vector<Message> &incoming = ctx->messages;
        if (!session->history.empty())
        {
            if (is_prefix(session->history, incoming))
            {
                ctx->messages = diff_messages(session->history, incoming);
            }
            else
            {
                session->history.clear();
                session->model_ctx.reset();
                ctx->messages = incoming;
            }
        }
        else
        {
            ctx->messages = incoming;
        }
    }

    result.ok = true;
    result.request.ctx = std::move(ctx);
    result.status = 200;
    result.type.clear();
    result.code.clear();
    result.message.clear();
    return result;
}
