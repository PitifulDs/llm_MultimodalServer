#include "serving/service/HealthService.h"

nlohmann::json HealthService::BuildHealth(const PlatformRuntimeSnapshot &snapshot) const
{
    return {
        {"status", "ok"},
        {"uptime_ms", snapshot.uptime_ms},
        {"requests_total", snapshot.requests_total},
        {"requests_in_flight", snapshot.requests_in_flight},
    };
}
