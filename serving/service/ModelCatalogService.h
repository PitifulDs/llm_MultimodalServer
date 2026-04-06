#pragma once

#include <string>
#include <vector>

#include "engine/ModelRegistry.h"
#include "serving/core/ModelCapability.h"

class ModelCatalogService
{
public:
    std::vector<ModelInfo> ListModels() const;
    bool HasModel(const std::string &model_id) const;
    bool SupportsCapability(const std::string &model_id,
                            ModelCapability capability,
                            const std::string &preferred_backend = "") const;
};
