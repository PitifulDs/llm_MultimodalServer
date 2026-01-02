#pragma once

#include <memory>
#include <string>
#include "engine/LLMEngine.h"

class EngineFactory {
public:
    static std::shared_ptr<LLMEngine> Create(const std::string& model_);
};