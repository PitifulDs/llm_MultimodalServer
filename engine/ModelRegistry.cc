#include "engine/ModelRegistry.h"

#include "utils/json.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
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

std::string to_lower_copy(std::string s)
{
    for (char &ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool ends_with(const std::string &value, const std::string &suffix)
{
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string normalize_backend_name(std::string backend)
{
    backend = to_lower_copy(std::move(backend));
    if (backend == "rpc" || backend == "remote" || backend == "worker" || backend == "stackflow")
        return "stackflow";
    if (backend == "local" || backend == "llama")
        return "local";
    return "";
}

std::string display_model_name(const std::string &name)
{
    if (ends_with(name, "-remote"))
        return name.substr(0, name.size() - 7);
    return name;
}

const json *find_model_entry(const json &models, const std::string &requested, const std::string &preferred_backend)
{
    auto find_object = [&](const std::string &name) -> const json * {
        auto it = models.find(name);
        if (it != models.end() && it->is_object())
            return &(*it);
        return nullptr;
    };

    const std::string normalized_backend = normalize_backend_name(preferred_backend);
    if (normalized_backend == "stackflow")
    {
        if (const json *entry = find_object(requested + "-remote"))
            return entry;
        if (const json *entry = find_object(requested))
            return entry;
        if (ends_with(requested, "-remote"))
        {
            if (const json *entry = find_object(requested))
                return entry;
            return find_object(display_model_name(requested));
        }
        return nullptr;
    }

    if (normalized_backend == "local")
    {
        if (const json *entry = find_object(requested))
            return entry;
        if (ends_with(requested, "-remote"))
            return find_object(display_model_name(requested));
        return nullptr;
    }

    return find_object(requested);
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

ModelSpec ModelRegistry::Resolve(const std::string &model_name, const std::string &preferred_backend)
{
    const json cfg = load_config();
    const std::string requested = model_name.empty() ? GetDefaultModel() : model_name;
    const std::string normalized_backend = normalize_backend_name(preferred_backend);

    if (cfg.contains("models") && cfg["models"].is_object())
    {
        const auto &models = cfg["models"];
        const json *entry = find_model_entry(models, requested, normalized_backend);
        if (entry)
        {
            const json &source = *entry;
            if (normalized_backend == "stackflow")
                return build_stackflow_spec(requested, source, cfg);
            if (normalized_backend == "local")
                return build_local_llama_spec(requested, source, cfg);

            const std::string backend = json_or_default(source, "backend", std::string(""));
            const std::string engine = json_or_default(source, "engine", std::string(""));

            if (backend == "stackflow" || engine == "stackflow")
                return build_stackflow_spec(requested, source, cfg);
            if (backend == "local" || engine == "llama" || engine.empty())
                return build_local_llama_spec(requested, source, cfg);
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

    if (normalized_backend == "stackflow")
        return build_stackflow_spec(requested, json::object(), cfg);
    if (normalized_backend == "local")
        return build_local_llama_spec(requested, json::object(), cfg);

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
    const auto infos = ListModelInfos();
    std::vector<std::string> names;
    names.reserve(infos.size());
    for (const auto &info : infos)
        names.push_back(info.id);
    if (names.empty())
        names.push_back("llama");
    return names;
}

std::vector<ModelInfo> ModelRegistry::ListModelInfos()
{
    const json cfg = load_config();
    std::map<std::string, ModelInfo> infos;
    const std::string default_model = display_model_name(GetDefaultModel());

    if (cfg.contains("models") && cfg["models"].is_object())
    {
        for (auto it = cfg["models"].begin(); it != cfg["models"].end(); ++it)
        {
            if (!it->is_object())
                continue;

            const std::string id = display_model_name(it.key());
            auto &info = infos[id];
            info.id = id;
            info.is_default = id == default_model;

            const std::string backend = normalize_backend_name(json_or_default(*it, "backend", std::string("")));
            const std::string engine = normalize_backend_name(json_or_default(*it, "engine", std::string("")));

            if (backend == "dummy" || engine == "dummy")
                continue;

            // 语义：model 是逻辑模型名，backend 是请求级推理后端开关。
            // /v1/models 的 backends 表示“网关支持的调用模式”，不作为模型级硬限制。
            info.has_local = true;
            info.has_rpc = true;
        }
    }

    if (infos.find(default_model) == infos.end())
    {
        auto &info = infos[default_model];
        info.id = default_model;
        info.is_default = true;
        const ModelSpec spec = Resolve(default_model);
        if (spec.valid && spec.engine != "dummy")
        {
            info.has_local = true;
            info.has_rpc = true;
        }
    }

    std::vector<ModelInfo> out;
    out.reserve(infos.size());
    for (auto &[_, info] : infos)
        out.push_back(info);
    return out;
}
