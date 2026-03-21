#include "engine/ModelRegistry.h"

#include "utils/json.hpp"

#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace
{
std::string get_env_string(const char *name, const char *fallback)
{
    const char *val = std::getenv(name);
    return (val && *val) ? std::string(val) : std::string(fallback);
}

int get_env_int(const char *name, int fallback)
{
    const char *val = std::getenv(name);
    if (!val || !*val)
        return fallback;
    try
    {
        return std::stoi(val);
    }
    catch (...)
    {
        return fallback;
    }
}

bool get_env_bool(const char *name, bool fallback)
{
    const char *val = std::getenv(name);
    if (!val || !*val)
        return fallback;
    return std::string(val) != "0";
}

json load_config()
{
    const char *cfg_path = std::getenv("CONFIG_PATH");
    const std::string path = (cfg_path && *cfg_path) ? std::string(cfg_path) : std::string("config.json");

    std::ifstream in(path);
    if (!in.is_open())
        return json::object();

    try
    {
        return json::parse(in);
    }
    catch (...)
    {
        return json::object();
    }
}

std::string json_or_default(const json &j, const char *key, const std::string &fallback)
{
    if (j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return fallback;
}

int json_or_default(const json &j, const char *key, int fallback)
{
    if (j.contains(key) && j[key].is_number_integer())
        return j[key].get<int>();
    return fallback;
}

bool json_or_default(const json &j, const char *key, bool fallback)
{
    if (j.contains(key) && j[key].is_boolean())
        return j[key].get<bool>();
    if (j.contains(key) && j[key].is_number_integer())
        return j[key].get<int>() != 0;
    return fallback;
}

ModelSpec build_stackflow_spec(const std::string &requested_name, const json &source, const json &cfg)
{
    ModelSpec spec;
    spec.valid = true;
    spec.requested_name = requested_name;
    spec.backend = "stackflow";
    spec.engine = "stackflow";
    spec.stackflow_host = json_or_default(source, "host",
        json_or_default(source, "stackflow_host",
            get_env_string("STACKFLOW_HOST", json_or_default(cfg, "stackflow_host", std::string("127.0.0.1")).c_str())));
    spec.stackflow_port = json_or_default(source, "port",
        json_or_default(source, "stackflow_port",
            get_env_int("STACKFLOW_PORT", json_or_default(cfg, "stackflow_port", 10001))));
    spec.stackflow_unit = json_or_default(source, "unit",
        json_or_default(source, "stackflow_unit",
            get_env_string("STACKFLOW_UNIT", json_or_default(cfg, "stackflow_unit", std::string("llm")).c_str())));
    spec.stackflow_response_format = json_or_default(source, "response_format",
        get_env_string("STACKFLOW_RESPONSE_FORMAT", "llm.utf-8"));
    spec.stackflow_response_format_stream = json_or_default(source, "response_format_stream",
        get_env_string("STACKFLOW_RESPONSE_FORMAT_STREAM", "llm.utf-8.stream"));
    spec.stackflow_timeout_ms = json_or_default(source, "timeout_ms",
        json_or_default(source, "stackflow_timeout_ms",
            get_env_int("STACKFLOW_TIMEOUT_MS", json_or_default(cfg, "stackflow_timeout_ms", 10000))));
    spec.stackflow_infer_timeout_ms = json_or_default(source, "infer_timeout_ms",
        json_or_default(source, "stackflow_infer_timeout_ms",
            get_env_int("STACKFLOW_INFER_TIMEOUT_MS", json_or_default(cfg, "stackflow_infer_timeout_ms", 0))));
    spec.stackflow_reuse_work_id = json_or_default(source, "reuse_work_id",
        json_or_default(source, "stackflow_reuse_work_id",
            get_env_bool("STACKFLOW_REUSE_WORK_ID", json_or_default(cfg, "stackflow_reuse_work_id", true))));
    spec.stackflow_serialize_reuse = json_or_default(source, "serialize_reuse",
        json_or_default(source, "stackflow_serialize_reuse",
            get_env_bool("STACKFLOW_SERIALIZE_REUSE", json_or_default(cfg, "stackflow_serialize_reuse", true))));
    return spec;
}

ModelSpec build_local_llama_spec(const std::string &requested_name, const json &source, const json &cfg)
{
    ModelSpec spec;
    spec.valid = true;
    spec.requested_name = requested_name;
    spec.backend = "local";
    spec.engine = "llama";
    spec.model_path = json_or_default(source, "model_path",
        json_or_default(source, "llama_model_path",
            get_env_string("LLAMA_MODEL_PATH",
                json_or_default(cfg, "llama_model_path",
                    std::string("models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf")).c_str())));
    return spec;
}
} // namespace

ModelSpec ModelRegistry::Resolve(const std::string &model_name)
{
    const json cfg = load_config();
    const std::string requested = model_name.empty() ? GetDefaultModel() : model_name;

    if (cfg.contains("models") && cfg["models"].is_object())
    {
        const auto &models = cfg["models"];
        auto it = models.find(requested);
        if (it != models.end() && it->is_object())
        {
            const json &entry = *it;
            const std::string backend = json_or_default(entry, "backend", std::string(""));
            const std::string engine = json_or_default(entry, "engine", std::string(""));

            if (backend == "stackflow" || engine == "stackflow")
                return build_stackflow_spec(requested, entry, cfg);
            if (backend == "local" || engine == "llama" || engine.empty())
                return build_local_llama_spec(requested, entry, cfg);
            if (backend == "dummy" || engine == "dummy")
            {
                ModelSpec spec;
                spec.valid = true;
                spec.requested_name = requested;
                spec.backend = "dummy";
                spec.engine = "dummy";
                return spec;
            }
        }
    }

    if (requested == "dummy")
    {
        ModelSpec spec;
        spec.valid = true;
        spec.requested_name = requested;
        spec.backend = "dummy";
        spec.engine = "dummy";
        return spec;
    }

    const std::string backend = get_env_string("SERVING_BACKEND",
        json_or_default(cfg, "serving_backend", std::string("local")).c_str());

    if (backend == "stackflow" || requested == "stackflow")
        return build_stackflow_spec(requested, json::object(), cfg);

    return build_local_llama_spec(requested, json::object(), cfg);
}

std::string ModelRegistry::GetDefaultModel()
{
    const char *env = std::getenv("DEFAULT_MODEL");
    if (env && *env)
        return std::string(env);

    const json cfg = load_config();
    if (cfg.contains("default_model") && cfg["default_model"].is_string())
        return cfg["default_model"].get<std::string>();

    return "llama";
}

std::vector<std::string> ModelRegistry::ListModels()
{
    const json cfg = load_config();
    std::set<std::string> names;

    if (cfg.contains("models") && cfg["models"].is_object())
    {
        for (auto it = cfg["models"].begin(); it != cfg["models"].end(); ++it)
        {
            names.insert(it.key());
        }
    }

    names.insert(GetDefaultModel());

    if (names.empty())
        names.insert("llama");

    return std::vector<std::string>(names.begin(), names.end());
}
