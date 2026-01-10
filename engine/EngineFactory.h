#pragma once

#include <memory>
#include <string>

class ModelEngine;
class EngineFactory {
public:
    static std::shared_ptr<ModelEngine> Create(const std::string &model_);
};