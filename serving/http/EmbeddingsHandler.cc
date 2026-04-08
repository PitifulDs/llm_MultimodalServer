#include "HttpGateway.h"

#include "HttpGatewayInternal.h"
#include "http_types.h"
#include "serving/service/RequestLogging.h"
#include "utils/json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <string>

using json = nlohmann::json;

namespace
{
    struct EmbeddingsRequestParseResult
    {
        bool ok = false;
        int status = 500;
        std::string message;
        std::string type;
        std::string code;
        EmbeddingsRequest request;
    };

    EmbeddingsRequestParseResult ParseEmbeddingsRequestBody(const std::string &body_text,
                                                           const std::string &default_model,
                                                           const std::string &request_id)
    {
        EmbeddingsRequestParseResult result;
        result.status = 400;
        result.type = "invalid_request_error";

        json body;
        try
        {
            body = json::parse(body_text.empty() ? "{}" : body_text);
        }
        catch (...)
        {
            result.message = "invalid json";
            result.code = "invalid_json";
            return result;
        }

        if (!body.contains("input"))
        {
            result.message = "input is required";
            result.code = "invalid_input";
            return result;
        }

        if (body["input"].is_string())
        {
            result.request.input.push_back(body["input"].get<std::string>());
        }
        else if (body["input"].is_array())
        {
            for (const auto &item : body["input"])
            {
                if (!item.is_string())
                {
                    result.message = "input array must contain only strings";
                    result.code = "invalid_input";
                    return result;
                }
                result.request.input.push_back(item.get<std::string>());
            }
        }
        else
        {
            result.message = "input must be a string or array of strings";
            result.code = "invalid_input";
            return result;
        }

        result.request.request_id = request_id;
        result.request.model =
            (body.contains("model") && body["model"].is_string())
                ? body["model"].get<std::string>()
                : default_model;

        if (body.contains("encoding_format"))
        {
            if (!body["encoding_format"].is_string())
            {
                result.message = "encoding_format must be string";
                result.code = "unsupported_encoding_format";
                return result;
            }
            result.request.encoding_format = body["encoding_format"].get<std::string>();
            std::transform(result.request.encoding_format.begin(),
                           result.request.encoding_format.end(),
                           result.request.encoding_format.begin(),
                           [](unsigned char ch)
                           { return static_cast<char>(std::tolower(ch)); });
        }

        std::string preferred_backend;
        if (body.contains("inference_backend") && body["inference_backend"].is_string())
            preferred_backend = body["inference_backend"].get<std::string>();
        else if (body.contains("backend") && body["backend"].is_string())
            preferred_backend = body["backend"].get<std::string>();
        result.request.inference_backend = http_gateway_internal::NormalizeBackendName(std::move(preferred_backend));

        result.ok = true;
        return result;
    }

    json BuildEmbeddingsJson(const EmbeddingsResponse &response)
    {
        json data = json::array();
        for (const auto &item : response.data)
        {
            data.push_back({
                {"object", "embedding"},
                {"index", item.index},
                {"embedding", item.embedding},
            });
        }

        return {
            {"object", "list"},
            {"data", data},
            {"model", response.model},
            {"usage",
             {{"prompt_tokens", response.usage.prompt_tokens},
              {"total_tokens", response.usage.total_tokens}}}
        };
    }
}

void HttpGateway::HandleEmbeddings(const HttpRequest &req, HttpResponse &res)
{
    const auto start_time = std::chrono::steady_clock::now();
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    in_flight_.fetch_add(1, std::memory_order_relaxed);

    const std::string request_id = http_gateway_internal::GenRequestId();
    auto parsed = ParseEmbeddingsRequestBody(req.body, http_gateway_internal::GetDefaultModel(), request_id);
    if (!parsed.ok)
    {
        WriteError(res, parsed.status, parsed.message, parsed.type, parsed.code);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              ModelCapability::Embeddings,
                              "",
                              FinishReason::error,
                              parsed.status,
                              parsed.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    LogPlatformRequest("start", RequestLogRecord{
                                    parsed.request.request_id,
                                    "/v1/embeddings",
                                    parsed.request.model,
                                    parsed.request.inference_backend,
                                    std::string(ToString(parsed.request.capability)),
                                    "",
                                    -1,
                                    -1,
                                    "",
                                    0,
                                    "",
                                    0,
                                    0,
                                    0,
                                    false});

    if (!embeddings_service_)
    {
        const PlatformError error{
            PlatformErrorKind::Internal,
            "embeddings_service_unavailable",
            "embeddings service unavailable"};
        WriteError(res, error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
                              FinishReason::error,
                              error.HttpStatus(),
                              error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    const EmbeddingsError validation_error = embeddings_service_->ValidateRequest(parsed.request);
    if (validation_error.HasError())
    {
        WriteError(res, validation_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
                              FinishReason::error,
                              validation_error.HttpStatus(),
                              validation_error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    std::shared_ptr<void> request_lease;
    const PlatformError limit_error = AcquireRequestLease(parsed.request.model, "", request_lease);
    if (limit_error.HasError())
    {
        WriteError(res, limit_error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              parsed.request.inference_backend,
                              parsed.request.capability,
                              "",
                              FinishReason::error,
                              limit_error.HttpStatus(),
                              limit_error.code,
                              0,
                              dur_ms,
                              0,
                              0,
                              0,
                              false);
        RecordFinish(FinishReason::error, dur_ms);
        return;
    }

    parsed.request.cancelled = std::make_shared<std::atomic<bool>>(false);
    if (request_timeout_ms_ > 0)
        parsed.request.deadline = start_time + std::chrono::milliseconds(request_timeout_ms_);
    const auto watchdog_done = std::make_shared<std::atomic<bool>>(false);
    http_gateway_internal::ScopedWatchdogDone stop_watchdog{watchdog_done};
    http_gateway_internal::StartSyncCancelWatchdog(parsed.request.cancelled, watchdog_done, [&res]()
    {
        return res.IsAlive();
    });

    const auto result = embeddings_service_->Run(parsed.request);
    if (!res.IsAlive())
    {
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                              parsed.request.capability,
                              "",
                              FinishReason::cancelled,
                              499,
                              "request_cancelled",
                              0,
                              dur_ms,
                              result.response.usage.prompt_tokens,
                              result.response.usage.completion_tokens,
                              result.response.usage.total_tokens,
                              false);
        RecordFinish(FinishReason::cancelled, dur_ms);
        return;
    }

    if (result.error.HasError())
    {
        WriteError(res, result.error);
        const FinishReason finish_reason = http_gateway_internal::FinishReasonFromError(result.error);
        const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count();
        RecordGovernedRequest(parsed.request.request_id,
                              "/v1/embeddings",
                              parsed.request.model,
                              result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                              parsed.request.capability,
                              "",
                              finish_reason,
                              result.error.HttpStatus(),
                              result.error.code,
                              0,
                              dur_ms,
                              result.response.usage.prompt_tokens,
                              result.response.usage.completion_tokens,
                              result.response.usage.total_tokens,
                              false);
        RecordFinish(finish_reason, dur_ms);
        return;
    }

    const json out = BuildEmbeddingsJson(result.response);
    res.SetStatus(200, "OK");
    res.SetHeader("Content-Type", "application/json");
    res.SetHeader("Connection", "close");
    res.Write(out.dump(-1, ' ', false, json::error_handler_t::replace));
    res.End();

    const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time)
                            .count();
    RecordGovernedRequest(parsed.request.request_id,
                          "/v1/embeddings",
                          result.response.model,
                          result.resolved_backend.empty() ? parsed.request.inference_backend : result.resolved_backend,
                          parsed.request.capability,
                          "",
                          FinishReason::stop,
                          200,
                          "",
                          0,
                          dur_ms,
                          result.response.usage.prompt_tokens,
                          result.response.usage.completion_tokens,
                          result.response.usage.total_tokens,
                          false);
    RecordFinish(FinishReason::stop, dur_ms);
}
