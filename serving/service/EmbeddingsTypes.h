#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "serving/core/CompletionTypes.h"
#include "serving/core/ModelCapability.h"
#include "serving/service/PlatformError.h"

struct EmbeddingsRequest
{
    std::string request_id;
    std::string model;
    std::string inference_backend;
    std::vector<std::string> input;
    std::string encoding_format = "float";
    ModelCapability capability = ModelCapability::Embeddings;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
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

using EmbeddingsErrorKind = PlatformErrorKind;
using EmbeddingsError = PlatformError;
