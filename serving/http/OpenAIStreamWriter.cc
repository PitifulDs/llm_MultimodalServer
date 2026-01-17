#include "OpenAIStreamWriter.h"
#include "utils/json.hpp"

using json = nlohmann::json;

OpenAIStreamWriter::OpenAIStreamWriter(const std::string &request_id, const std::string &model, WriteFn write)
                                        : request_id_(request_id), model_(model), write_(write)       
{
}

void OpenAIStreamWriter::OnChunk(const StreamChunk &chunk)
{
    json j;
    j["id"] = request_id_;
    j["object"] = "chat.completion.chunk";
    j["model"] = model_;

    json choices;
    choices["index"] = 0;

    if(!chunk.is_finished){
       choices["delta"]["content"] = chunk.delta; 
       choices["finish_reason"] = nullptr;
    }else{
        choices["delta"] = json::object();
        choices["finish_reason"] = "stop";
    }

    j["choices"] = json::array({choices});

    write_("data: " + j.dump() + "\n\n");

    if(chunk.is_finished) {
        write_("data: [DONE]\n\n");
    }
}
