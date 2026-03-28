#pragma once

#include <vector>
#include <string>

struct ModelSpec
{
    bool valid = false;
    std::string requested_name;
    std::string backend;
    std::string engine;
    std::string model_path;

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
};
