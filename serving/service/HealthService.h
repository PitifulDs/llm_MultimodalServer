#pragma once

#include "serving/service/StatusTypes.h"
#include "utils/json.hpp"

class HealthService
{
public:
    nlohmann::json BuildHealth(const PlatformRuntimeSnapshot &snapshot) const;
};
