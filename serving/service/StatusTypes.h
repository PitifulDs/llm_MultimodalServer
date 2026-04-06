#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/ModelRegistry.h"

struct PlatformRuntimeSnapshot
{
    int64_t uptime_ms = 0;
    int64_t requests_total = 0;
    int64_t requests_in_flight = 0;
    int64_t requests_stream_total = 0;
    int64_t requests_error_total = 0;
    int64_t requests_cancelled_total = 0;
};

struct BackendRuntimeSnapshot
{
    std::string backend;
    size_t model_count = 0;
    size_t loaded_engine_count = 0;
    size_t queue_length = 0;
    int64_t requests_total = 0;
    int64_t requests_error_total = 0;
    int64_t requests_cancelled_total = 0;
    std::vector<std::string> capabilities;
    std::string last_error;
    int64_t timeout_total = 0;
    int64_t cancelled_total = 0;
};
