#include "HttpGatewayInternal.h"

#include "HttpUtils.h"
#include "engine/ModelRegistry.h"
#include "serving/core/ServingContext.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

namespace http_gateway_internal
{
    size_t GetWorkerThreads()
    {
        const char *env = std::getenv("WORKER_THREADS");
        if (!env || !*env)
            return 4;
        try
        {
            const int value = std::stoi(env);
            return value > 0 ? static_cast<size_t>(value) : 4;
        }
        catch (...)
        {
            return 4;
        }
    }

    std::string GetDefaultModel()
    {
        return ModelRegistry::GetDefaultModel();
    }

    int GetDefaultMaxTokens()
    {
        const char *env = std::getenv("DEFAULT_MAX_TOKENS");
        if (!env || !*env)
            return 0;
        try
        {
            const int value = std::stoi(env);
            return value > 0 ? value : 0;
        }
        catch (...)
        {
            return 0;
        }
    }

    std::string GetEnvString(const char *name, const char *def_val)
    {
        const char *env = std::getenv(name);
        if (!env || !*env)
            return std::string(def_val);
        return std::string(env);
    }

    int GetEnvInt(const char *name, int def)
    {
        const char *env = std::getenv(name);
        if (!env || !*env)
            return def;
        try
        {
            const int value = std::stoi(env);
            return value > 0 ? value : def;
        }
        catch (...)
        {
            return def;
        }
    }

    bool GetEnvBool(const char *name, bool def)
    {
        const char *env = std::getenv(name);
        if (!env || !*env)
            return def;

        std::string value(env);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                       { return static_cast<char>(std::tolower(ch)); });
        if (value == "1" || value == "true" || value == "yes")
            return true;
        if (value == "0" || value == "false" || value == "no")
            return false;
        return def;
    }

    std::string GenRequestId()
    {
        static std::atomic<uint64_t> sequence{0};
        return "req-" + std::to_string(++sequence);
    }

    std::filesystem::path DetectRepoRoot()
    {
        if (const char *cfg = std::getenv("CONFIG_PATH"))
        {
            if (*cfg)
            {
                std::error_code ec;
                auto cfg_path = std::filesystem::weakly_canonical(std::filesystem::path(cfg), ec);
                if (ec)
                    cfg_path = std::filesystem::path(cfg).lexically_normal();
                if (cfg_path.has_parent_path())
                    return cfg_path.parent_path();
            }
        }
        return std::filesystem::current_path();
    }

    const char *FinishReasonToStr(FinishReason reason)
    {
        return http_utils::finish_reason_to_str(reason);
    }

    std::string NormalizeBackendName(std::string backend)
    {
        std::transform(backend.begin(), backend.end(), backend.begin(), [](unsigned char ch)
                       { return static_cast<char>(std::tolower(ch)); });
        if (backend == "rpc" || backend == "remote" || backend == "worker" || backend == "stackflow")
            return "stackflow";
        if (backend == "local" || backend == "llama")
            return "local";
        return "";
    }

    int64_t ParseInt64OrDefault(const std::string &value, int64_t default_value)
    {
        if (value.empty())
            return default_value;
        try
        {
            return std::stoll(value);
        }
        catch (...)
        {
            return default_value;
        }
    }

    bool HasDeadline(std::chrono::steady_clock::time_point deadline)
    {
        return deadline != std::chrono::steady_clock::time_point::max();
    }

    void StartSyncCancelWatchdog(const std::shared_ptr<std::atomic<bool>> &cancel_flag,
                                 const std::shared_ptr<std::atomic<bool>> &done,
                                 const std::function<bool()> &is_client_alive)
    {
        if (!cancel_flag || !done)
            return;

        std::thread([cancel_flag, done, is_client_alive]
        {
            while (!done->load(std::memory_order_acquire))
            {
                if (!is_client_alive())
                {
                    cancel_flag->store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
    }

    PlatformError BuildChatPlatformError(const ServingContext &ctx)
    {
        const std::string error_code =
            ctx.params.count("error_code") ? ctx.params.at("error_code") : std::string();

        if (error_code == "invalid_request" ||
            error_code == "model_not_found" ||
            error_code == "capability_not_supported" ||
            error_code == "rag_invalid_request" ||
            error_code == "rag_no_user_query")
        {
            return {
                PlatformErrorKind::InvalidRequest,
                error_code,
                ctx.error_message.empty() ? "invalid chat request" : ctx.error_message};
        }

        if (error_code == "rag_index_missing" ||
            error_code == "rag_unavailable")
        {
            return {
                PlatformErrorKind::ServiceUnavailable,
                error_code,
                ctx.error_message.empty() ? "backend unavailable" : ctx.error_message};
        }

        if (IsPlatformRateLimitCode(error_code) ||
            ctx.error_message.find("queue full") != std::string::npos)
        {
            return {
                PlatformErrorKind::RateLimit,
                error_code.empty() ? "queue_full" : error_code,
                ctx.error_message.empty() ? "engine overloaded" : ctx.error_message};
        }

        return BuildPlatformErrorFromCode(error_code,
                                          ctx.error_message,
                                          "invalid chat request",
                                          "chat request cancelled",
                                          "request timed out",
                                          "backend unavailable",
                                          "engine overloaded",
                                          "engine error");
    }

    FinishReason FinishReasonFromError(const PlatformError &error)
    {
        return error.kind == PlatformErrorKind::Cancelled ? FinishReason::cancelled : FinishReason::error;
    }

    std::string BuildExperimentalApiDisabledMessage(const std::string &route,
                                                    const std::string &env_name)
    {
        return route + " is an experimental compatibility API and is disabled by default; set " +
               env_name + "=1 to re-enable it explicitly";
    }

    nlohmann::json BuildChatCompletionJson(const std::string &request_id,
                                           const ChatResponse &response)
    {
        nlohmann::json out = {
            {"id", "chatcmpl-" + request_id},
            {"object", "chat.completion"},
            {"created", static_cast<int>(std::time(nullptr))},
            {"model", response.model},
            {"choices",
             {{{"index", 0},
               {"message", {{"role", "assistant"}, {"content", response.output_text}}},
               {"logprobs", nullptr},
               {"finish_reason", FinishReasonToStr(response.finish_reason)}}}},
            {"usage",
             {{"prompt_tokens", response.usage.prompt_tokens},
              {"completion_tokens", response.usage.completion_tokens},
              {"total_tokens", response.usage.total_tokens}}}
        };

        if (!response.references.is_null())
            out["references"] = response.references;
        if (!response.retrieval.is_null())
            out["retrieval"] = response.retrieval;
        if (!response.agent_result.is_null())
            out["agent_result"] = response.agent_result;
        if (!response.subqueries.is_null())
            out["subqueries"] = response.subqueries;
        if (!response.evidence.is_null())
            out["evidence"] = response.evidence;
        if (!response.agent_trace.is_null())
            out["agent_trace"] = response.agent_trace;

        return out;
    }
}
