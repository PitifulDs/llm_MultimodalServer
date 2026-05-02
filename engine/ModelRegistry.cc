#include "engine/ModelRegistry.h"

#include "utils/json.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace
{
struct ResolvedModelRoute
{
    bool valid = false;
    std::string model_id;
    std::string backend;
    std::string default_backend;
    std::vector<std::string> capabilities;
    const json *model_entry = nullptr;
    const json *backend_entry = nullptr;
};

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

std::string json_string_from_sources(const json &primary,
                                     const json &secondary,
                                     const char *key,
                                     const char *alt_key,
                                     const std::string &fallback)
{
    const std::string with_primary = json_or_default(primary, key, std::string());
    if (!with_primary.empty())
        return with_primary;

    if (alt_key)
    {
        const std::string with_primary_alt = json_or_default(primary, alt_key, std::string());
        if (!with_primary_alt.empty())
            return with_primary_alt;
    }

    const std::string with_secondary = json_or_default(secondary, key, std::string());
    if (!with_secondary.empty())
        return with_secondary;

    if (alt_key)
    {
        const std::string with_secondary_alt = json_or_default(secondary, alt_key, std::string());
        if (!with_secondary_alt.empty())
            return with_secondary_alt;
    }

    return fallback;
}

int json_int_from_sources(const json &primary,
                          const json &secondary,
                          const char *key,
                          const char *alt_key,
                          int fallback)
{
    if (primary.contains(key) && primary[key].is_number_integer())
        return primary[key].get<int>();
    if (alt_key && primary.contains(alt_key) && primary[alt_key].is_number_integer())
        return primary[alt_key].get<int>();
    if (secondary.contains(key) && secondary[key].is_number_integer())
        return secondary[key].get<int>();
    if (alt_key && secondary.contains(alt_key) && secondary[alt_key].is_number_integer())
        return secondary[alt_key].get<int>();
    return fallback;
}

bool json_bool_from_sources(const json &primary,
                            const json &secondary,
                            const char *key,
                            const char *alt_key,
                            bool fallback)
{
    if (primary.contains(key))
        return json_or_default(primary, key, fallback);
    if (alt_key && primary.contains(alt_key))
        return json_or_default(primary, alt_key, fallback);
    if (secondary.contains(key))
        return json_or_default(secondary, key, fallback);
    if (alt_key && secondary.contains(alt_key))
        return json_or_default(secondary, alt_key, fallback);
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

void add_unique_string(std::vector<std::string> &values, const std::string &value)
{
    if (value.empty())
        return;
    if (std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

bool contains_string(const std::vector<std::string> &values, const std::string &target)
{
    return std::find(values.begin(), values.end(), target) != values.end();
}

bool legacy_entry_declares_backend(const json &entry, const std::string &backend)
{
    const std::string declared_backend = normalize_backend_name(json_or_default(entry, "backend", std::string()));
    if (!declared_backend.empty())
        return declared_backend == backend;

    const std::string engine = to_lower_copy(json_or_default(entry, "engine", std::string()));
    if (backend == "stackflow")
        return engine == "stackflow";
    if (backend == "local")
        return engine == "llama";
    return false;
}

std::vector<std::string> restrict_capabilities_for_backend(const std::string &backend,
                                                           const std::vector<std::string> &capabilities)
{
    if (backend != "stackflow")
        return capabilities;

    std::vector<std::string> filtered;
    if (contains_string(capabilities, "chat"))
        filtered.push_back("chat");
    return filtered;
}

std::vector<std::string> extract_capabilities(const json &node, const std::vector<std::string> &fallback)
{
    if (!node.is_array())
        return fallback;

    std::vector<std::string> out;
    for (const auto &item : node)
    {
        if (!item.is_string())
            continue;

        ModelCapability capability;
        if (ParseModelCapability(item.get<std::string>(), capability))
            add_unique_string(out, ToString(capability));
    }

    return out.empty() ? fallback : out;
}

std::vector<std::string> merge_capabilities(const std::vector<std::string> &lhs,
                                            const std::vector<std::string> &rhs)
{
    std::vector<std::string> merged = lhs;
    for (const auto &value : rhs)
        add_unique_string(merged, value);
    return merged;
}

std::string infer_legacy_backend_name(const std::string &model_name, const json &entry)
{
    if (ends_with(model_name, "-remote"))
        return "stackflow";

    const std::string backend = normalize_backend_name(json_or_default(entry, "backend", std::string()));
    if (!backend.empty())
        return backend;

    const std::string engine = to_lower_copy(json_or_default(entry, "engine", std::string()));
    if (engine == "stackflow")
        return "stackflow";
    if (engine == "dummy")
        return "dummy";
    return "local";
}

bool is_structured_model_entry(const json &entry)
{
    return entry.is_object() &&
           (entry.contains("backends") || entry.contains("capabilities") || entry.contains("default_backend"));
}

const json *find_object_entry(const json &models, const std::string &name)
{
    auto it = models.find(name);
    if (it != models.end() && it->is_object())
        return &(*it);
    return nullptr;
}

std::vector<std::pair<std::string, const json *>> collect_structured_backends(const json &model_entry)
{
    std::vector<std::pair<std::string, const json *>> backends;
    if (!model_entry.contains("backends") || !model_entry["backends"].is_object())
        return backends;

    for (auto it = model_entry["backends"].begin(); it != model_entry["backends"].end(); ++it)
    {
        if (!it->is_object())
            continue;

        const std::string normalized = normalize_backend_name(it.key());
        if (normalized.empty())
            continue;

        bool replaced = false;
        for (auto &item : backends)
        {
            if (item.first == normalized)
            {
                item.second = &(*it);
                replaced = true;
                break;
            }
        }
        if (!replaced)
            backends.push_back({normalized, &(*it)});
    }
    return backends;
}

std::string select_structured_default_backend(const json &model_entry,
                                              const std::vector<std::pair<std::string, const json *>> &backends)
{
    std::string default_backend = normalize_backend_name(json_or_default(model_entry, "default_backend", std::string()));
    if (!default_backend.empty())
    {
        for (const auto &item : backends)
        {
            if (item.first == default_backend)
                return default_backend;
        }
    }

    if (!backends.empty())
    {
        for (const auto &item : backends)
        {
            if (item.first == "local")
                return item.first;
        }
        return backends.front().first;
    }

    return default_backend;
}

std::vector<std::string> capabilities_for_structured_backend(const json &model_entry, const json &backend_entry)
{
    const std::vector<std::string> model_caps = extract_capabilities(
        model_entry.contains("capabilities") ? model_entry["capabilities"] : json::array(),
        std::vector<std::string>{"chat"});

    return extract_capabilities(
        backend_entry.contains("capabilities") ? backend_entry["capabilities"] : json::array(),
        model_caps);
}

std::vector<std::string> capabilities_for_structured_backend(const std::string &backend,
                                                             const json &model_entry,
                                                             const json &backend_entry)
{
    return restrict_capabilities_for_backend(
        backend,
        capabilities_for_structured_backend(model_entry, backend_entry));
}

ResolvedModelRoute resolve_structured_model_route(const json &models,
                                                  const std::string &requested_name,
                                                  const std::string &preferred_backend)
{
    const std::string model_id = display_model_name(requested_name);
    const json *model_entry = find_object_entry(models, model_id);
    if (!model_entry || !is_structured_model_entry(*model_entry))
        return {};

    const auto backends = collect_structured_backends(*model_entry);
    if (backends.empty())
        return {};

    std::string selected_backend = normalize_backend_name(preferred_backend);
    const json *backend_entry = nullptr;
    if (!selected_backend.empty())
    {
        for (const auto &item : backends)
        {
            if (item.first == selected_backend)
            {
                backend_entry = item.second;
                break;
            }
        }
        if (!backend_entry)
            return {};
    }
    else
    {
        selected_backend = select_structured_default_backend(*model_entry, backends);
        for (const auto &item : backends)
        {
            if (item.first == selected_backend)
            {
                backend_entry = item.second;
                break;
            }
        }
        if (!backend_entry)
        {
            selected_backend = backends.front().first;
            backend_entry = backends.front().second;
        }
    }

    ResolvedModelRoute route;
    route.valid = backend_entry != nullptr;
    route.model_id = model_id;
    route.backend = selected_backend;
    route.default_backend = select_structured_default_backend(*model_entry, backends);
    route.capabilities = capabilities_for_structured_backend(selected_backend, *model_entry, *backend_entry);
    route.model_entry = model_entry;
    route.backend_entry = backend_entry;
    return route;
}

bool has_configured_model(const json &models, const std::string &requested)
{
    if (find_object_entry(models, requested))
        return true;

    const std::string logical = display_model_name(requested);
    if (find_object_entry(models, logical))
        return true;

    return find_object_entry(models, logical + "-remote") != nullptr;
}

const json *find_legacy_model_entry(const json &models,
                                    const std::string &requested,
                                    const std::string &preferred_backend)
{
    const auto find_object = [&](const std::string &name) -> const json * {
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
        if (const json *entry = find_object(requested);
            entry && (ends_with(requested, "-remote") || legacy_entry_declares_backend(*entry, "stackflow")))
            return entry;
        if (ends_with(requested, "-remote"))
        {
            if (const json *entry = find_object(requested);
                entry && legacy_entry_declares_backend(*entry, "stackflow"))
                return entry;
            const json *logical_entry = find_object(display_model_name(requested));
            return (logical_entry && legacy_entry_declares_backend(*logical_entry, "stackflow")) ? logical_entry : nullptr;
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

ModelSpec build_stackflow_spec(const std::string &requested_name,
                               const std::string &model_id,
                               const std::string &default_backend,
                               const std::vector<std::string> &capabilities,
                               const json &source,
                               const json &fallback_source,
                               const json &cfg)
{
    ModelSpec spec;
    spec.valid = true;
    spec.requested_name = requested_name;
    spec.model_id = model_id.empty() ? display_model_name(requested_name) : model_id;
    spec.backend = "stackflow";
    spec.default_backend = default_backend.empty() ? "stackflow" : default_backend;
    spec.engine = "stackflow";
    spec.capabilities = capabilities.empty() ? std::vector<std::string>{"chat"} : capabilities;
    spec.stackflow_host = json_string_from_sources(source, fallback_source, "host", "stackflow_host",
        get_env_string("STACKFLOW_HOST", json_or_default(cfg, "stackflow_host", std::string("127.0.0.1")).c_str()));
    spec.stackflow_port = json_int_from_sources(source, fallback_source, "port", "stackflow_port",
        get_env_int("STACKFLOW_PORT", json_or_default(cfg, "stackflow_port", 10001)));
    spec.stackflow_unit = json_string_from_sources(source, fallback_source, "unit", "stackflow_unit",
        get_env_string("STACKFLOW_UNIT", json_or_default(cfg, "stackflow_unit", std::string("llm")).c_str()));
    spec.stackflow_response_format = json_string_from_sources(source, fallback_source, "response_format", nullptr,
        get_env_string("STACKFLOW_RESPONSE_FORMAT", "llm.utf-8"));
    spec.stackflow_response_format_stream = json_string_from_sources(source, fallback_source, "response_format_stream", nullptr,
        get_env_string("STACKFLOW_RESPONSE_FORMAT_STREAM", "llm.utf-8.stream"));
    spec.stackflow_timeout_ms = json_int_from_sources(source, fallback_source, "timeout_ms", "stackflow_timeout_ms",
        get_env_int("STACKFLOW_TIMEOUT_MS", json_or_default(cfg, "stackflow_timeout_ms", 10000)));
    spec.stackflow_infer_timeout_ms = json_int_from_sources(source, fallback_source, "infer_timeout_ms", "stackflow_infer_timeout_ms",
        get_env_int("STACKFLOW_INFER_TIMEOUT_MS", json_or_default(cfg, "stackflow_infer_timeout_ms", 0)));
    spec.stackflow_reuse_work_id = json_bool_from_sources(source, fallback_source, "reuse_work_id", "stackflow_reuse_work_id",
        get_env_bool("STACKFLOW_REUSE_WORK_ID", json_or_default(cfg, "stackflow_reuse_work_id", true)));
    spec.stackflow_serialize_reuse = json_bool_from_sources(source, fallback_source, "serialize_reuse", "stackflow_serialize_reuse",
        get_env_bool("STACKFLOW_SERIALIZE_REUSE", json_or_default(cfg, "stackflow_serialize_reuse", true)));
    return spec;
}

ModelSpec build_local_llama_spec(const std::string &requested_name,
                                 const std::string &model_id,
                                 const std::string &default_backend,
                                 const std::vector<std::string> &capabilities,
                                 const json &source,
                                 const json &fallback_source,
                                 const json &cfg)
{
    ModelSpec spec;
    spec.valid = true;
    spec.requested_name = requested_name;
    spec.model_id = model_id.empty() ? display_model_name(requested_name) : model_id;
    spec.backend = "local";
    spec.default_backend = default_backend.empty() ? "local" : default_backend;
    spec.engine = "llama";
    spec.capabilities = capabilities.empty() ? std::vector<std::string>{"chat"} : capabilities;
    spec.model_path = json_string_from_sources(source, fallback_source, "model_path", "llama_model_path",
        get_env_string("LLAMA_MODEL_PATH",
            json_or_default(cfg, "llama_model_path",
                std::string("models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf")).c_str()));
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

        const ResolvedModelRoute structured = resolve_structured_model_route(models, requested, normalized_backend);
        if (structured.valid && structured.backend_entry && structured.model_entry)
        {
            if (structured.backend == "stackflow")
                return build_stackflow_spec(requested,
                                            structured.model_id,
                                            structured.default_backend,
                                            structured.capabilities,
                                            *structured.backend_entry,
                                            *structured.model_entry,
                                            cfg);
            if (structured.backend == "local")
                return build_local_llama_spec(requested,
                                              structured.model_id,
                                              structured.default_backend,
                                              structured.capabilities,
                                              *structured.backend_entry,
                                              *structured.model_entry,
                                              cfg);
        }

        const json *entry = find_legacy_model_entry(models, requested, normalized_backend);
        if (entry && !is_structured_model_entry(*entry))
        {
            const std::string backend = infer_legacy_backend_name(
                normalized_backend == "stackflow" ? requested + "-remote" : requested,
                *entry);
            const std::string model_id = display_model_name(requested);

            if (backend == "stackflow")
                return build_stackflow_spec(requested, model_id, backend, {"chat"}, *entry, *entry, cfg);
            if (backend == "local")
                return build_local_llama_spec(requested, model_id, backend, {"chat"}, *entry, *entry, cfg);
            if (backend == "dummy")
            {
                ModelSpec spec;
                spec.valid = true;
                spec.requested_name = requested;
                spec.model_id = model_id;
                spec.backend = "dummy";
                spec.default_backend = "dummy";
                spec.engine = "dummy";
                spec.capabilities = {"chat"};
                return spec;
            }
        }

        if (!normalized_backend.empty() && has_configured_model(models, requested))
            return {};
    }

    if (normalized_backend == "stackflow")
        return build_stackflow_spec(requested, display_model_name(requested), "stackflow", {"chat"}, json::object(), json::object(), cfg);
    if (normalized_backend == "local")
        return build_local_llama_spec(requested, display_model_name(requested), "local", {"chat"}, json::object(), json::object(), cfg);

    if (requested == "dummy")
    {
        ModelSpec spec;
        spec.valid = true;
        spec.requested_name = requested;
        spec.model_id = requested;
        spec.backend = "dummy";
        spec.default_backend = "dummy";
        spec.engine = "dummy";
        spec.capabilities = {"chat"};
        return spec;
    }

    const std::string backend = get_env_string("SERVING_BACKEND",
        json_or_default(cfg, "serving_backend", std::string("local")).c_str());

    if (backend == "stackflow" || requested == "stackflow")
        return build_stackflow_spec(requested, display_model_name(requested), "stackflow", {"chat"}, json::object(), json::object(), cfg);

    return build_local_llama_spec(requested, display_model_name(requested), "local", {"chat"}, json::object(), json::object(), cfg);
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

            if (is_structured_model_entry(*it))
            {
                const auto backends = collect_structured_backends(*it);
                info.default_backend = select_structured_default_backend(*it, backends);
                info.capabilities = extract_capabilities(
                    it->contains("capabilities") ? (*it)["capabilities"] : json::array(),
                    std::vector<std::string>{});

                for (const auto &backend : backends)
                {
                    add_unique_string(info.backends, backend.first);
                    if (backend.first == "local")
                        info.has_local = true;
                    else if (backend.first == "stackflow")
                        info.has_rpc = true;

                    info.capabilities = merge_capabilities(
                        info.capabilities,
                        capabilities_for_structured_backend(backend.first, *it, *backend.second));
                }

                if (info.capabilities.empty())
                    info.capabilities.push_back("chat");
                continue;
            }

            const std::string backend = infer_legacy_backend_name(it.key(), *it);
            info.default_backend = backend == "stackflow" ? "stackflow" : "local";
            info.capabilities = {"chat"};
            if (backend == "stackflow")
            {
                info.has_rpc = true;
                add_unique_string(info.backends, "stackflow");
            }
            else if (backend == "local")
            {
                info.has_local = true;
                add_unique_string(info.backends, "local");
            }
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
            info.default_backend = spec.default_backend;
            info.capabilities = spec.capabilities.empty() ? std::vector<std::string>{"chat"} : spec.capabilities;
            if (spec.backend == "stackflow")
            {
                info.has_rpc = true;
                add_unique_string(info.backends, "stackflow");
            }
            else if (spec.backend == "local")
            {
                info.has_local = true;
                add_unique_string(info.backends, "local");
            }
        }
    }

    std::vector<ModelInfo> out;
    out.reserve(infos.size());
    for (auto &[_, info] : infos)
        out.push_back(info);
    return out;
}

bool ModelRegistry::SupportsCapability(const std::string &model_name,
                                       ModelCapability capability,
                                       const std::string &preferred_backend)
{
    const std::string target = ToString(capability);
    if (!preferred_backend.empty())
    {
        const ModelSpec spec = Resolve(model_name, preferred_backend);
        return spec.valid && contains_string(spec.capabilities, target);
    }

    const std::string logical_model = display_model_name(model_name);
    for (const auto &info : ListModelInfos())
    {
        if (info.id == logical_model)
            return contains_string(info.capabilities, target);
    }
    return false;
}
