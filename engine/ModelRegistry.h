#pragma once

#include <string>
#include <vector>

#include "serving/core/ModelCapability.h"

struct ModelSpec
{
    bool valid = false;
    std::string requested_name;
    std::string model_id;
    std::string backend;
    std::string default_backend;
    std::string engine;
    std::string model_path;
    std::vector<std::string> capabilities;

    std::string stackflow_host;
    int stackflow_port = 10001;
    std::string stackflow_unit;
    std::string stackflow_response_format;
    std::string stackflow_response_format_stream;
    int stackflow_timeout_ms = 10000;
    int stackflow_infer_timeout_ms = 0;
    bool stackflow_reuse_work_id = true;
    bool stackflow_serialize_reuse = true;
};

struct ModelInfo
{
    std::string id;
    bool is_default = false;
    std::string default_backend;
    std::vector<std::string> capabilities;
    std::vector<std::string> backends;
    bool has_local = false;
    bool has_rpc = false;
};

class ModelRegistry
{
public:
    static ModelSpec Resolve(const std::string &model_name, const std::string &preferred_backend = "");
    static std::string GetDefaultModel();
    static std::vector<std::string> ListModels();
    static std::vector<ModelInfo> ListModelInfos();
    static bool SupportsCapability(const std::string &model_name,
                                   ModelCapability capability,
                                   const std::string &preferred_backend = "");
};
