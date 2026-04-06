#include "serving/service/ModelCatalogService.h"

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

bool ModelCatalogService::SupportsCapability(const std::string &model_id,
                                             ModelCapability capability,
                                             const std::string &preferred_backend) const
{
    return ModelRegistry::SupportsCapability(model_id, capability, preferred_backend);
}
