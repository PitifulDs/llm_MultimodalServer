#pragma once

#include <functional>
#include <memory>
#include <string>

#include "serving/core/agent/ToolRegistry.h"
#include "utils/json.hpp"

struct ServingContext;
class EngineExecutor;
class CodeAnalysisPlanner;
class CodeAnalysisEvidenceStore;

class AgentExecutor
{
public:
    struct Options
    {
        int default_max_steps = 4;
        size_t max_tool_output_chars = 4000;
        std::string repo_root = ".";
        std::string docs_root = ".";
        std::string config_path = "config.json";
        std::function<std::string(const nlohmann::json &)> search_kb_handler;
        std::function<std::string(const nlohmann::json &)> open_chunk_handler;
    };

    AgentExecutor(EngineExecutor &executor, Options options);

    void SetStatusProvider(std::function<std::string()> provider);
    void Run(const std::shared_ptr<ServingContext> &ctx);

private:
    void RunCodeAnalysis(const std::shared_ptr<ServingContext> &ctx,
                         const std::vector<std::string> &allowed_tools,
                         int max_steps);
    void RunGenericAgent(const std::shared_ptr<ServingContext> &ctx,
                         const std::vector<std::string> &allowed_tools,
                         int max_steps);

    EngineExecutor &executor_;
    Options options_;
    ToolRegistry tool_registry_;
    std::function<std::string()> status_provider_;
};
