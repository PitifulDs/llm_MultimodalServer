#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "serving/core/CompletionTypes.h"
#include "serving/core/ModelCapability.h"

struct EmbeddingsRequest
{
    std::string request_id;
    std::string model;
    std::string inference_backend;
    std::vector<std::string> input;
    std::string encoding_format = "float";
    ModelCapability capability = ModelCapability::Embeddings;
};

struct EmbeddingData
{
    size_t index = 0;
    std::vector<float> embedding;
};

struct EmbeddingsResponse
{
    std::string model;
    std::vector<EmbeddingData> data;
    UsageInfo usage;
    std::string error_message;
};

enum class EmbeddingsErrorKind
{
    None,
    InvalidRequest,
    ServiceUnavailable,
    RateLimit,
    Internal
};

struct EmbeddingsError
{
    EmbeddingsErrorKind kind = EmbeddingsErrorKind::None;
    std::string code;
    std::string message;

    bool HasError() const
    {
        return kind != EmbeddingsErrorKind::None;
    }
};
