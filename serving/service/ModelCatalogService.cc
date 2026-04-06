#include "serving/service/ModelCatalogService.h"

std::vector<ModelInfo> ModelCatalogService::ListModels() const
{
    return ModelRegistry::ListModelInfos();
}

bool ModelCatalogService::SupportsCapability(const std::string &model_id,
                                             ModelCapability capability,
                                             const std::string &preferred_backend) const
{
    return ModelRegistry::SupportsCapability(model_id, capability, preferred_backend);
}
