#include "OpenAIStreamWriter.h"
#include "utils/json.hpp"

#include <utility>

using json = nlohmann::json;

OpenAIStreamWriter::OpenAIStreamWriter(const std::string &request_id, const std::string &model, WriteFn write)
                                        : request_id_(request_id), model_(model), write_(write)       
{
}

static const char *finish_reason_to_str(FinishReason r)
{
    switch (r)
    {
    case FinishReason::stop:
        return "stop";
    case FinishReason::length:
        return "length";
    case FinishReason::cancelled:
        return "cancelled"; // 如需更贴 OpenAI，可改成 "stop"
    case FinishReason::error:
    default:
        return "error";
    }
}

namespace {
constexpr const char *kUtf8Replacement = "\xEF\xBF\xBD";

bool is_utf8_continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

std::pair<std::string, std::string> split_utf8_prefix(const std::string &input, bool flush_incomplete)
{
    std::string out;
    out.reserve(input.size());
    const size_t n = input.size();
    size_t i = 0;
    while (i < n)
    {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80)
        {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        size_t need = 0;
        if (c >= 0xC2 && c <= 0xDF)
            need = 2;
        else if (c >= 0xE0 && c <= 0xEF)
            need = 3;
        else if (c >= 0xF0 && c <= 0xF4)
            need = 4;
        else
        {
            out.append(kUtf8Replacement);
            ++i;
            continue;
        }

        if (i + need > n)
        {
            if (flush_incomplete)
            {
                out.append(kUtf8Replacement);
                break;
            }
            return {out, input.substr(i)};
        }

        bool valid = true;
        for (size_t k = 1; k < need; ++k)
        {
            if (!is_utf8_continuation(static_cast<unsigned char>(input[i + k])))
            {
                valid = false;
                break;
            }
        }

        if (!valid)
        {
            out.append(kUtf8Replacement);
            ++i;
            continue;
        }

        out.append(input, i, need);
        i += need;
    }

    return {out, std::string()};
}
} // namespace

void OpenAIStreamWriter::OnChunk(const StreamChunk &chunk)
{
    std::string safe_delta;
    if (!chunk.delta.empty())
    {
        const std::string combined = pending_bytes_ + chunk.delta;
        auto [prefix, pending] = split_utf8_prefix(combined, false);
        safe_delta = std::move(prefix);
        pending_bytes_ = std::move(pending);
    }

    if (chunk.is_finished && !pending_bytes_.empty())
    {
        auto [flushed, _] = split_utf8_prefix(pending_bytes_, true);
        safe_delta.append(flushed);
        pending_bytes_.clear();
    }

    json j;
    j["id"] = "chatcmpl-" + request_id_;
    j["object"] = "chat.completion.chunk";
    j["created"] = static_cast<int>(std::time(nullptr));
    j["model"] = model_;

    json choice;
    choice["index"] = 0;

    if (!chunk.is_finished)
    {
        // 正常增量
        choice["delta"] = {{"content", safe_delta}};
        choice["finish_reason"] = nullptr;
    }
    else
    {
        // 结束 chunk：delta 为空对象
        choice["delta"] = json::object();
        choice["finish_reason"] = finish_reason_to_str(chunk.finish_reason);
    }

    j["choices"] = json::array({choice});

    write_("data: " + j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n");

    if (chunk.is_finished)
    {
        write_("data: [DONE]\n\n");
    }
}
