#include "serving/service/ModelCatalogService.h"

#include <algorithm>

namespace
{
std::string normalize_model_id(std::string model_id)
{
    static constexpr const char kRemoteSuffix[] = "-remote";
    static constexpr size_t kRemoteSuffixLen = sizeof(kRemoteSuffix) - 1;
    if (model_id.size() > kRemoteSuffixLen &&
        model_id.compare(model_id.size() - kRemoteSuffixLen, kRemoteSuffixLen, kRemoteSuffix) == 0)
    {
        model_id.resize(model_id.size() - kRemoteSuffixLen);
    }
    return model_id;
}

bool contains_capability(const std::vector<std::string> &capabilities, ModelCapability capability)
{
    const std::string target = ToString(capability);
    return std::find(capabilities.begin(), capabilities.end(), target) != capabilities.end();
}
} // namespace

std::vector<ModelInfo> ModelCatalogService::ListModels() const
{
    return ModelRegistry::ListModelInfos();
}

bool ModelCatalogService::HasModel(const std::string &model_id) const
{
    const std::string normalized = normalize_model_id(model_id);
    for (const auto &model : ListModels())
    {
        if (model.id == normalized)
            return true;
    }
    return false;
}

ModelSpec ModelCatalogService::ResolveModel(const std::string &model_id,
                                            ModelCapability capability,
                                            const std::string &preferred_backend) const
{
    const std::string normalized = normalize_model_id(model_id);

    if (!preferred_backend.empty())
    {
        const ModelSpec preferred = ModelRegistry::Resolve(normalized, preferred_backend);
        if (preferred.valid && contains_capability(preferred.capabilities, capability))
            return preferred;
        return {};
    }

    const ModelSpec resolved_default = ModelRegistry::Resolve(normalized);
    if (resolved_default.valid && contains_capability(resolved_default.capabilities, capability))
        return resolved_default;

    for (const auto &model : ListModels())
    {
        if (model.id != normalized)
            continue;

        std::vector<std::string> backends = model.backends;
        if (!model.default_backend.empty())
        {
            backends.erase(std::remove(backends.begin(), backends.end(), model.default_backend), backends.end());
            backends.insert(backends.begin(), model.default_backend);
        }

        for (const auto &backend : backends)
        {
            const ModelSpec spec = ModelRegistry::Resolve(normalized, backend);
            if (spec.valid && contains_capability(spec.capabilities, capability))
                return spec;
        }
        break;
    }

    return {};
}

bool ModelCatalogService::SupportsCapability(const std::string &model_id,
                                             ModelCapability capability,
                                             const std::string &preferred_backend) const
{
    return ModelRegistry::SupportsCapability(model_id, capability, preferred_backend);
}
