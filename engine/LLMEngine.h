#pragma once

#include <memory>

struct ServingContext;


class LLMEngine
{
public:
    virtual ~LLMEngine() = default;

    // non-stream
    virtual void Generate(ServingContext& ctx) = 0;

    // stream
    virtual void GenerateStream(ServingContext& ctx) = 0;

};

using LLMEnginePtr = std::shared_ptr<LLMEngine>;

    

