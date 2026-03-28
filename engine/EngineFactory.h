#pragma once

#include <memory>
#include <string>

class ModelEngine;
class EngineFactory {
public:
    static std::shared_ptr<ModelEngine> Create(const std::string &model_, const std::string &preferred_backend = "");

    static void ClearCache();
};
