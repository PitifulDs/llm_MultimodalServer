#include "serving/service/EmbeddingsService.h"

#include "engine/EngineFactory.h"
#include "engine/ModelRegistry.h"
#include "serving/backend/EmbeddingsEngineFacade.h"
#include "serving/service/ModelCatalogService.h"

#include <memory>

EmbeddingsService::EmbeddingsService(ModelCatalogService &model_catalog_service)
    : model_catalog_service_(model_catalog_service)
{
}

EmbeddingsError EmbeddingsService::ValidateRequest(const EmbeddingsRequest &request) const
{
    if (request.model.empty())
    {
        return {
            EmbeddingsErrorKind::InvalidRequest,
            "model_required",
            "model is required"};
    }

    if (request.input.empty())
    {
        return {
            EmbeddingsErrorKind::InvalidRequest,
            "invalid_input",
            "input must be a non-empty string or array of strings"};
    }

    if (!request.encoding_format.empty() && request.encoding_format != "float")
    {
        return {
            EmbeddingsErrorKind::InvalidRequest,
            "unsupported_encoding_format",
            "only encoding_format=float is currently supported"};
    }

    if (!model_catalog_service_.HasModel(request.model))
    {
        return {
            EmbeddingsErrorKind::InvalidRequest,
            "model_not_found",
            "model not found: " + request.model};
    }

    if (!model_catalog_service_.SupportsCapability(request.model, request.capability, request.inference_backend))
    {
        return {
            EmbeddingsErrorKind::InvalidRequest,
            "capability_not_supported",
            "model does not support capability: " + std::string(ToString(request.capability))};
    }

    return {};
}

EmbeddingsService::Result EmbeddingsService::Run(const EmbeddingsRequest &request) const
{
    Result result;
    result.response.model = request.model;

    result.error = ValidateRequest(request);
    if (result.error.HasError())
        return result;

    const ModelSpec spec = model_catalog_service_.ResolveModel(request.model,
                                                               request.capability,
                                                               request.inference_backend);
    if (!spec.valid)
    {
        result.error = {
            EmbeddingsErrorKind::ServiceUnavailable,
            "backend_not_available",
            "no backend is available for capability: " + std::string(ToString(request.capability))};
        return result;
    }
    result.resolved_backend = spec.backend;

    auto engine = EngineFactory::Create(request.model, spec.backend);
    if (!engine)
    {
        result.error = {
            EmbeddingsErrorKind::ServiceUnavailable,
            "backend_not_available",
            "failed to create backend engine for embeddings"};
        return result;
    }

    EmbeddingsEngineFacade facade(engine);
    if (!facade.Supports(request.capability))
    {
        result.error = {
            EmbeddingsErrorKind::ServiceUnavailable,
            "backend_not_available",
            "resolved backend does not implement embeddings"};
        return result;
    }

    result.response.model = spec.model_id.empty() ? request.model : spec.model_id;
    if (!facade.RunEmbeddings(request, result.response, result.response.error_message))
    {
        result.error = {
            EmbeddingsErrorKind::Internal,
            "internal_error",
            result.response.error_message.empty() ? "embeddings execution failed" : result.response.error_message};
        return result;
    }

    return result;
}
