#pragma once
#include <string>
#include <functional>
#include "serving/core/ServingContext.h"

class OpenAIStreamWriter {
public:
    using WriteFn = std::function<void(const std::string&)>;

    OpenAIStreamWriter(const std::string& request_id, const std::string& model, WriteFn write);

    // streaming
    void OnChunk(const StreamChunk& chunk);

private:
    std::string request_id_;
    std::string model_;
    WriteFn write_;

};