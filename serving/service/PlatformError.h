#pragma once

#include <string>

enum class PlatformErrorKind
{
    None,
    InvalidRequest,
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
