#pragma once

#include "serving/core/ServingContext.h"
#include "../../utils/json.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace http_utils
{
    using json = nlohmann::json;

    inline bool msg_equal(const Message &a, const Message &b)
    {
        return a.role == b.role && a.content == b.content;
    }

    inline bool is_prefix(const std::vector<Message> &history,
                          const std::vector<Message> &incoming)
    {
        if (history.size() > incoming.size())
            return false;
        for (size_t i = 0; i < history.size(); ++i)
        {
            if (!msg_equal(history[i], incoming[i]))
                return false;
        }
        return true;
    }

    inline std::vector<Message> diff_messages(const std::vector<Message> &history,
                                              const std::vector<Message> &incoming)
    {
        if (!is_prefix(history, incoming))
        {
            return incoming;
        }
        return std::vector<Message>(incoming.begin() + history.size(), incoming.end());
    }

    // FinishReason -> openai finish_reason
    inline const char *finish_reason_to_str(FinishReason r)
    {
        switch (r)
        {
        case FinishReason::stop:
            return "stop";
        case FinishReason::length:
            return "length";
        case FinishReason::cancelled:
            return "cancelled";
        case FinishReason::error:
        default:
            return "error";
        }
    }

    inline void set_param_if_number(const json &body, const char *key,
                                    std::unordered_map<std::string, std::string> &params)
    {
        if (body.contains(key) && body[key].is_number())
        {
            const double v = body[key].get<double>();
            params[key] = std::to_string(v);
        }
    }

    inline void set_param_if_int(const json &body, const char *key,
                                 std::unordered_map<std::string, std::string> &params)
    {
        if (body.contains(key) && body[key].is_number_integer())
        {
            const int v = body[key].get<int>();
            params[key] = std::to_string(v);
        }
    }

    inline void set_sampling_params(const json &body,
                                    std::unordered_map<std::string, std::string> &params)
    {
        set_param_if_int(body, "max_tokens", params);
        set_param_if_number(body, "temperature", params);
        set_param_if_number(body, "top_p", params);
        set_param_if_int(body, "top_k", params);
        set_param_if_number(body, "repeat_penalty", params);
        set_param_if_number(body, "repetition_penalty", params);
        set_param_if_number(body, "presence_penalty", params);
        set_param_if_number(body, "frequency_penalty", params);
        set_param_if_int(body, "repeat_last_n", params);
        set_param_if_int(body, "seed", params);
    }
} // namespace http_utils
