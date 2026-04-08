#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

enum class PlatformErrorKind
{
    None,
    InvalidRequest,
    Cancelled,
    Timeout,
    ServiceUnavailable,
    RateLimit,
    Internal
};

struct PlatformError
{
    PlatformErrorKind kind = PlatformErrorKind::None;
    std::string code;
    std::string message;

    bool HasError() const
    {
        return kind != PlatformErrorKind::None;
    }

    int HttpStatus() const
    {
        switch (kind)
        {
        case PlatformErrorKind::InvalidRequest:
            return 400;
        case PlatformErrorKind::Cancelled:
            return 499;
        case PlatformErrorKind::Timeout:
            return 504;
        case PlatformErrorKind::ServiceUnavailable:
            return 503;
        case PlatformErrorKind::RateLimit:
            return 429;
        case PlatformErrorKind::Internal:
        case PlatformErrorKind::None:
        default:
            return 500;
        }
    }

    const char *HttpType() const
    {
        switch (kind)
        {
        case PlatformErrorKind::InvalidRequest:
            return "invalid_request_error";
        case PlatformErrorKind::Cancelled:
            return "cancelled_error";
        case PlatformErrorKind::Timeout:
            return "timeout_error";
        case PlatformErrorKind::ServiceUnavailable:
            return "service_unavailable_error";
        case PlatformErrorKind::RateLimit:
            return "rate_limit_error";
        case PlatformErrorKind::Internal:
        case PlatformErrorKind::None:
        default:
            return "internal_error";
        }
    }
};

inline bool IsPlatformInvalidRequestCode(const std::string &code)
{
    return code == "invalid_request" ||
           code == "invalid_json" ||
           code == "model_required" ||
           code == "invalid_input" ||
           code == "invalid_encoding_format" ||
           code == "unsupported_encoding_format" ||
           code == "invalid_query" ||
           code == "invalid_documents" ||
           code == "invalid_top_n" ||
           code == "model_not_found" ||
           code == "capability_not_supported";
}

inline bool IsPlatformCancelledCode(const std::string &code)
{
    return code == "request_cancelled";
}

inline bool IsPlatformTimeoutCode(const std::string &code)
{
    return code == "backend_timeout" || code == "request_timeout";
}

inline bool IsPlatformServiceUnavailableCode(const std::string &code)
{
    return code == "backend_not_available";
}

inline bool IsPlatformRateLimitCode(const std::string &code)
{
    return code == "queue_full" || code == "queue_timeout" ||
           code == "rate_limit_global" || code == "rate_limit_model" ||
           code == "rate_limit_session";
}

inline bool HasPlatformDeadline(std::chrono::steady_clock::time_point deadline)
{
    return deadline != std::chrono::steady_clock::time_point::max();
}

inline bool PlatformRequestCancelled(const std::shared_ptr<std::atomic<bool>> &cancelled)
{
    return cancelled && cancelled->load(std::memory_order_acquire);
}

inline bool PlatformRequestTimedOut(std::chrono::steady_clock::time_point deadline)
{
    return HasPlatformDeadline(deadline) && std::chrono::steady_clock::now() >= deadline;
}

inline PlatformError BuildPlatformErrorFromCode(const std::string &code,
                                                const std::string &message,
                                                const char *invalid_request_message,
                                                const char *cancelled_message,
                                                const char *timeout_message,
                                                const char *service_unavailable_message,
                                                const char *rate_limit_message,
                                                const char *internal_message)
{
    if (IsPlatformInvalidRequestCode(code))
    {
        return {
            PlatformErrorKind::InvalidRequest,
            code,
            message.empty() ? std::string(invalid_request_message) : message};
    }

    if (IsPlatformCancelledCode(code))
    {
        return {
            PlatformErrorKind::Cancelled,
            code,
            message.empty() ? std::string(cancelled_message) : message};
    }

    if (IsPlatformTimeoutCode(code))
    {
        return {
            PlatformErrorKind::Timeout,
            code,
            message.empty() ? std::string(timeout_message) : message};
    }

    if (IsPlatformServiceUnavailableCode(code))
    {
        return {
            PlatformErrorKind::ServiceUnavailable,
            code,
            message.empty() ? std::string(service_unavailable_message) : message};
    }

    if (IsPlatformRateLimitCode(code))
    {
        return {
            PlatformErrorKind::RateLimit,
            code.empty() ? "queue_full" : code,
            message.empty() ? std::string(rate_limit_message) : message};
    }

    return {
        PlatformErrorKind::Internal,
        code.empty() ? "internal_error" : code,
        message.empty() ? std::string(internal_message) : message};
}

inline PlatformError BuildGovernedExecutionError(const std::shared_ptr<std::atomic<bool>> &cancelled,
                                                 std::chrono::steady_clock::time_point deadline,
                                                 const std::string &message,
                                                 const char *cancelled_message,
                                                 const char *timeout_message,
                                                 const char *internal_message)
{
    if (PlatformRequestCancelled(cancelled))
    {
        return {
            PlatformErrorKind::Cancelled,
            "request_cancelled",
            message.empty() ? std::string(cancelled_message) : message};
    }

    if (PlatformRequestTimedOut(deadline))
    {
        return {
            PlatformErrorKind::Timeout,
            "request_timeout",
            message.empty() ? std::string(timeout_message) : message};
    }

    return {
        PlatformErrorKind::Internal,
        "internal_error",
        message.empty() ? std::string(internal_message) : message};
}
