#include "serving/http/ChatRequestParser.h"

#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
#include "serving/core/SessionManager.h"
#include "serving/http/HttpUtils.h"
#include "utils/json.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace
{
bool get_agent_enabled(const json &body)
{
    return body.contains("agent") && body["agent"].is_boolean() && body["agent"].get<bool>();
}

std::string to_lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return s;
}

std::string get_inference_backend(const json &body)
{
    std::string backend;
    if (body.contains("inference_backend") && body["inference_backend"].is_string())
        backend = body["inference_backend"].get<std::string>();
    else if (body.contains("backend") && body["backend"].is_string())
        backend = body["backend"].get<std::string>();

    backend = to_lower_copy(std::move(backend));
    if (backend == "rpc" || backend == "remote" || backend == "worker" || backend == "stackflow")
        return "stackflow";
    if (backend == "local" || backend == "llama")
        return "local";
    return "";
}

std::string get_agent_mode(const json &body)
{
    if (!body.contains("agent_mode") || !body["agent_mode"].is_string())
        return "code_analysis";

    std::string mode = to_lower_copy(body["agent_mode"].get<std::string>());
    if (mode == "assistant" || mode == "code" || mode == "code_agent")
        mode = "code_analysis";
    if (mode != "code_analysis")
        return "code_analysis";
    return mode;
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

bool get_agent_debug(const json &body)
{
    return body.contains("agent_debug") && body["agent_debug"].is_boolean() && body["agent_debug"].get<bool>();
}

bool get_agent_include_trace(const json &body)
{
    return body.contains("agent_include_trace") && body["agent_include_trace"].is_boolean() && body["agent_include_trace"].get<bool>();
}

std::string get_agent_output_format(const json &body)
{
    if (!body.contains("agent_output_format") || !body["agent_output_format"].is_string())
        return "text";

    std::string format = to_lower_copy(body["agent_output_format"].get<std::string>());
    if (format != "structured")
        return "text";
    return format;
}

bool parse_rag_options(const json &body, RagOptions &out, std::string &error_message, std::string &error_code)
{
    out = RagOptions{};
    if (!body.contains("rag"))
        return true;

    if (!body["rag"].is_object())
    {
        error_message = "rag must be object";
        error_code = "invalid_rag";
        return false;
    }

    const auto &rag = body["rag"];
    if (rag.contains("enabled"))
    {
        if (!rag["enabled"].is_boolean())
        {
            error_message = "rag.enabled must be boolean";
            error_code = "invalid_rag_enabled";
            return false;
        }
        out.enabled = rag["enabled"].get<bool>();
    }

    if (rag.contains("kb"))
    {
        if (!rag["kb"].is_string())
        {
            error_message = "rag.kb must be string";
            error_code = "invalid_rag_kb";
            return false;
        }
        out.kb = to_lower_copy(rag["kb"].get<std::string>());
    }

    if (rag.contains("top_k"))
    {
        if (!rag["top_k"].is_number_integer())
        {
            error_message = "rag.top_k must be integer";
            error_code = "invalid_rag_top_k";
            return false;
        }
        out.top_k = rag["top_k"].get<int>();
        if (out.top_k < 0)
        {
            error_message = "rag.top_k must be >= 0";
            error_code = "invalid_rag_top_k";
            return false;
        }
    }

    if (rag.contains("mode"))
    {
        if (!rag["mode"].is_string())
        {
            error_message = "rag.mode must be string";
            error_code = "invalid_rag_mode";
            return false;
        }
        out.mode = to_lower_copy(rag["mode"].get<std::string>());
    }

    if (rag.contains("lexical_top_k"))
    {
        if (!rag["lexical_top_k"].is_number_integer())
        {
            error_message = "rag.lexical_top_k must be integer";
            error_code = "invalid_rag_lexical_top_k";
            return false;
        }
        out.lexical_top_k = rag["lexical_top_k"].get<int>();
        if (out.lexical_top_k < 0)
        {
            error_message = "rag.lexical_top_k must be >= 0";
            error_code = "invalid_rag_lexical_top_k";
            return false;
        }
    }

    if (rag.contains("vector_top_k"))
    {
        if (!rag["vector_top_k"].is_number_integer())
        {
            error_message = "rag.vector_top_k must be integer";
            error_code = "invalid_rag_vector_top_k";
            return false;
        }
        out.vector_top_k = rag["vector_top_k"].get<int>();
        if (out.vector_top_k < 0)
        {
            error_message = "rag.vector_top_k must be >= 0";
            error_code = "invalid_rag_vector_top_k";
            return false;
        }
    }

    if (rag.contains("fusion"))
    {
        if (!rag["fusion"].is_string())
        {
            error_message = "rag.fusion must be string";
            error_code = "invalid_rag_fusion";
            return false;
        }
        out.fusion = to_lower_copy(rag["fusion"].get<std::string>());
    }

    if (rag.contains("debug"))
    {
        if (!rag["debug"].is_boolean())
        {
            error_message = "rag.debug must be boolean";
            error_code = "invalid_rag_debug";
            return false;
        }
        out.debug = rag["debug"].get<bool>();
    }

    if (rag.contains("return_references"))
    {
        if (!rag["return_references"].is_boolean())
        {
            error_message = "rag.return_references must be boolean";
            error_code = "invalid_rag_return_references";
            return false;
        }
        out.return_references = rag["return_references"].get<bool>();
    }

    if (out.enabled && out.kb.empty())
    {
        error_message = "rag.kb is required when rag.enabled=true";
        error_code = "invalid_rag_kb";
        return false;
    }

    return true;
}

} // namespace

ChatRequestParseResult ParseChatRequestBody(const std::string &body_text,
                                           bool stream_fallback,
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
    ctx->inference_backend = get_inference_backend(body);
    bool stream = stream_fallback;
    if (body.contains("stream"))
    {
        if (!body["stream"].is_boolean())
        {
            result.message = "stream must be boolean";
            result.code = "invalid_stream";
            return result;
        }
        stream = body["stream"].get<bool>();
    }
    ctx->stream = stream;
    ctx->is_chat = true;
    ctx->use_agent = get_agent_enabled(body);
    ctx->agent_mode = ctx->use_agent ? get_agent_mode(body) : std::string();
    ctx->agent_max_steps = get_agent_max_steps(body);
    ctx->agent_tools = get_agent_tools(body);
    ctx->agent_debug = ctx->use_agent ? get_agent_debug(body) : false;
    ctx->agent_include_trace = ctx->use_agent ? (ctx->agent_debug || get_agent_include_trace(body)) : false;
    ctx->agent_output_format = ctx->use_agent ? get_agent_output_format(body) : std::string("text");

    std::string rag_error_message;
    std::string rag_error_code;
    if (!parse_rag_options(body, ctx->rag_options, rag_error_message, rag_error_code))
    {
        result.message = rag_error_message;
        result.code = rag_error_code;
        return result;
    }

    if (body.contains("session_id") && body["session_id"].is_string())
        ctx->session_id = body["session_id"].get<std::string>();
    else
        ctx->session_id = request_id;

    ctx->session = session_mgr.getOrCreate(ctx->session_id, ctx->model, ctx->inference_backend);

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

    result.ok = true;
    result.request.ctx = std::move(ctx);
    result.status = 200;
    result.type.clear();
    result.code.clear();
    result.message.clear();
    return result;
}
