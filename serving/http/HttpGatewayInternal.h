#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "serving/service/ChatTypes.h"
#include "serving/service/PlatformError.h"
#include "utils/json.hpp"

struct ServingContext;
enum class FinishReason;

namespace http_gateway_internal
{
    size_t GetWorkerThreads();
    std::string GetDefaultModel();
    int GetDefaultMaxTokens();
    std::string GetEnvString(const char *name, const char *def_val = "");
    int GetEnvInt(const char *name, int def);
    bool GetEnvBool(const char *name, bool def);
    std::string GenRequestId();
    std::filesystem::path DetectRepoRoot();
    const char *FinishReasonToStr(FinishReason reason);
    std::string NormalizeBackendName(std::string backend);
    int64_t ParseInt64OrDefault(const std::string &value, int64_t default_value = 0);
    bool HasDeadline(std::chrono::steady_clock::time_point deadline);
    void StartContextDeadlineWatchdog(const std::weak_ptr<ServingContext> &weak_ctx,
                                      const std::string &timeout_message = "request timed out");
    void StartSyncCancelWatchdog(const std::shared_ptr<std::atomic<bool>> &cancel_flag,
                                 const std::shared_ptr<std::atomic<bool>> &done,
                                 const std::function<bool()> &is_client_alive);

    struct ScopedWatchdogDone
    {
        std::shared_ptr<std::atomic<bool>> done;

        ~ScopedWatchdogDone()
        {
            if (done)
                done->store(true, std::memory_order_release);
        }
    };

    PlatformError BuildChatPlatformError(const ServingContext &ctx);
    FinishReason FinishReasonFromError(const PlatformError &error);
    std::string BuildExperimentalApiDisabledMessage(const std::string &route,
                                                    const std::string &env_name);
    nlohmann::json BuildChatCompletionJson(const std::string &request_id,
                                           const ChatResponse &response);
}
