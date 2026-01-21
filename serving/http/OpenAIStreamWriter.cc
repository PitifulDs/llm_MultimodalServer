#include "OpenAIStreamWriter.h"
#include "utils/json.hpp"

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

void OpenAIStreamWriter::OnChunk(const StreamChunk &chunk)
{
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
        choice["delta"] = {{"content", chunk.delta}};
        choice["finish_reason"] = nullptr;
    }
    else
    {
        // 结束 chunk：delta 为空对象
        choice["delta"] = json::object();
        choice["finish_reason"] = finish_reason_to_str(chunk.finish_reason);
    }

    j["choices"] = json::array({choice});

    write_("data: " + j.dump() + "\n\n");

    if (chunk.is_finished)
    {
        write_("data: [DONE]\n\n");
    }
}