#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "serving/core/agent/ToolRegistry.h"
#include "utils/json.hpp"

struct BuiltinToolsOptions
{
    std::string repo_root = ".";
    std::string docs_root = ".";
    std::string config_path = "config.json";
    size_t max_tool_output_chars = 4000;
    std::function<std::string(const nlohmann::json &)> search_kb_handler;
    std::function<std::string(const nlohmann::json &)> open_chunk_handler;
};

void RegisterBuiltinTools(ToolRegistry &registry,
                          const BuiltinToolsOptions &options,
                          std::function<std::string()> status_provider);
