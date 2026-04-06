#include "serving/service/AdminStatusService.h"

#include <algorithm>
#include <map>
#include <string>

namespace
{
std::string to_gateway_backend_name(const std::string &backend)
{
    if (backend == "stackflow")
        return "rpc";
    return backend;
}

std::vector<std::string> collect_available_backends(const ModelInfo &model,
                                                    const std::map<std::string, BackendRuntimeSnapshot> &runtime_by_backend)
{
    if (runtime_by_backend.empty())
        return model.backends;

    std::vector<std::string> available;
    for (const auto &backend : model.backends)
    {
        const auto it = runtime_by_backend.find(backend);
        if (it == runtime_by_backend.end())
        {
            available.push_back(backend);
            continue;
        }
        available.push_back(backend);
    }
    return available;
}
} // namespace

nlohmann::json AdminStatusService::BuildModelsStatus(const std::vector<ModelInfo> &models,
                                                     const std::vector<BackendRuntimeSnapshot> &backend_runtime) const
{
    std::map<std::string, BackendRuntimeSnapshot> runtime_by_backend;
    for (const auto &item : backend_runtime)
        runtime_by_backend[item.backend] = item;

    nlohmann::json items = nlohmann::json::array();
    for (const auto &model : models)
    {
        const auto available_backends = collect_available_backends(model, runtime_by_backend);
        const bool available = !available_backends.empty();
        items.push_back({
            {"id", model.id},
            {"registered", true},
            {"available", available},
            {"default_backend", model.default_backend},
            {"gateway_default_backend", to_gateway_backend_name(model.default_backend)},
            {"capabilities", model.capabilities},
            {"declared_backends", model.backends},
            {"available_backends", available_backends},
            {"failure_summary", ""},
        });
    }

    return {
        {"object", "list"},
        {"data", items},
    };
}

nlohmann::json AdminStatusService::BuildBackendsStatus(const std::vector<ModelInfo> &models,
                                                       const std::vector<BackendRuntimeSnapshot> &backend_runtime,
                                                       const PlatformRuntimeSnapshot &platform_runtime) const
{
    std::map<std::string, BackendRuntimeSnapshot> runtime_by_backend;
    for (const auto &item : backend_runtime)
        runtime_by_backend[item.backend] = item;

    for (const auto &model : models)
    {
        for (const auto &backend : model.backends)
        {
            auto &snapshot = runtime_by_backend[backend];
            snapshot.backend = backend;
            snapshot.model_count += 1;
            for (const auto &capability : model.capabilities)
            {
                if (std::find(snapshot.capabilities.begin(), snapshot.capabilities.end(), capability) == snapshot.capabilities.end())
                    snapshot.capabilities.push_back(capability);
            }
        }
    }

    nlohmann::json items = nlohmann::json::array();
    for (const auto &[backend, runtime] : runtime_by_backend)
    {
        items.push_back({
            {"backend", backend},
            {"gateway_backend", to_gateway_backend_name(backend)},
            {"connected", true},
            {"model_count", runtime.model_count},
            {"capabilities", runtime.capabilities},
            {"loaded_engine_count", runtime.loaded_engine_count},
            {"queue_length", runtime.queue_length},
            {"requests_total", runtime.requests_total},
            {"requests_error_total", runtime.requests_error_total},
            {"requests_cancelled_total", runtime.requests_cancelled_total},
            {"requests_timeout_total", runtime.requests_timeout_total},
            {"requests_rate_limited_total", runtime.requests_rate_limited_total},
            {"last_error", runtime.last_error},
            {"timeout_total", runtime.timeout_total},
            {"cancelled_total", runtime.cancelled_total},
            {"prompt_tokens_total", runtime.prompt_tokens_total},
            {"completion_tokens_total", runtime.completion_tokens_total},
            {"total_tokens_total", runtime.total_tokens_total},
            {"requests_in_flight", platform_runtime.requests_in_flight},
        });
    }

    return {
        {"object", "list"},
        {"data", items},
    };
}
