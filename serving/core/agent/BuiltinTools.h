#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "serving/core/agent/ToolRegistry.h"

struct BuiltinToolsOptions
{
    std::string docs_root = ".";
    std::string config_path = "config.json";
    size_t max_tool_output_chars = 4000;
};

void RegisterBuiltinTools(ToolRegistry &registry,
                          const BuiltinToolsOptions &options,
                          std::function<std::string()> status_provider);
