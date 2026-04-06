#include "serving/service/AdminStatusService.h"

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
} // namespace

nlohmann::json AdminStatusService::BuildModelsStatus(const std::vector<ModelInfo> &models) const
{
    nlohmann::json items = nlohmann::json::array();
    for (const auto &model : models)
    {
        const bool available = model.has_local || model.has_rpc;
        items.push_back({
            {"id", model.id},
            {"registered", true},
            {"available", available},
            {"default_backend", model.default_backend},
            {"gateway_default_backend", to_gateway_backend_name(model.default_backend)},
            {"capabilities", model.capabilities},
            {"declared_backends", model.backends},
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
            if (!runtime_by_backend.count(backend))
            {
                BackendRuntimeSnapshot snapshot;
                snapshot.backend = backend;
                runtime_by_backend.emplace(backend, snapshot);
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
            {"loaded_engine_count", runtime.loaded_engine_count},
            {"queue_length", runtime.queue_length},
            {"last_error", runtime.last_error},
            {"timeout_total", runtime.timeout_total},
            {"cancelled_total", runtime.cancelled_total},
            {"requests_in_flight", platform_runtime.requests_in_flight},
        });
    }

    return {
        {"object", "list"},
        {"data", items},
    };
}
