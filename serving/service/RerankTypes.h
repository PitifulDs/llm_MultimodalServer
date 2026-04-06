#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "serving/core/CompletionTypes.h"
#include "serving/core/ModelCapability.h"
#include "serving/service/PlatformError.h"

struct RerankRequest
{
    std::string request_id;
    std::string model;
    std::string inference_backend;
    std::string query;
    std::vector<std::string> documents;
    int top_n = 0;
    ModelCapability capability = ModelCapability::Rerank;
};

struct RerankResultItem
{
    size_t index = 0;
    std::string document;
    double relevance_score = 0.0;
};

struct RerankResponse
{
    std::string model;
    std::vector<RerankResultItem> data;
    UsageInfo usage;
    std::string error_message;
};

using RerankErrorKind = PlatformErrorKind;
using RerankError = PlatformError;
