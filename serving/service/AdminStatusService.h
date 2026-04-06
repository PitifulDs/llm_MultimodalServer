#pragma once

#include <vector>

#include "serving/service/StatusTypes.h"
#include "utils/json.hpp"

class AdminStatusService
{
public:
    nlohmann::json BuildModelsStatus(const std::vector<ModelInfo> &models,
                                     const std::vector<BackendRuntimeSnapshot> &backend_runtime) const;
    nlohmann::json BuildBackendsStatus(const std::vector<ModelInfo> &models,
                                       const std::vector<BackendRuntimeSnapshot> &backend_runtime,
                                       const PlatformRuntimeSnapshot &platform_runtime) const;
};
