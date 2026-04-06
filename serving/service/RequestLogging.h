#pragma once

#include <cstdint>
#include <string>

#include <glog/logging.h>

struct RequestLogRecord
{
    std::string request_id;
    std::string api;
    std::string model;
    std::string backend;
    std::string capability;
    std::string session_id;
    int64_t queue_wait_ms = -1;
    int64_t run_ms = -1;
    std::string finish_reason;
    int status_code = 0;
    std::string error_code;
    bool stream = false;
};

inline std::string NormalizeRequestLogValue(const std::string &value, const char *fallback = "-")
{
    return value.empty() ? std::string(fallback) : value;
}

inline void LogPlatformRequest(const char *phase, const RequestLogRecord &record)
{
    LOG(INFO) << "[request] phase=" << NormalizeRequestLogValue(phase)
              << " request_id=" << NormalizeRequestLogValue(record.request_id)
              << " api=" << NormalizeRequestLogValue(record.api)
              << " model=" << NormalizeRequestLogValue(record.model)
              << " backend=" << NormalizeRequestLogValue(record.backend, "auto")
              << " capability=" << NormalizeRequestLogValue(record.capability)
              << " session_id=" << NormalizeRequestLogValue(record.session_id)
              << " queue_wait_ms=" << record.queue_wait_ms
              << " run_ms=" << record.run_ms
              << " finish_reason=" << NormalizeRequestLogValue(record.finish_reason)
              << " status_code=" << record.status_code
              << " error_code=" << NormalizeRequestLogValue(record.error_code)
              << " stream=" << (record.stream ? 1 : 0);
}
