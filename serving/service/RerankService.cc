#include "serving/service/RerankService.h"

#include "engine/EngineFactory.h"
#include "engine/ModelRegistry.h"
#include "serving/backend/RerankEngineFacade.h"
#include "serving/service/ModelCatalogService.h"

#include <memory>

RerankService::RerankService(ModelCatalogService &model_catalog_service)
    : model_catalog_service_(model_catalog_service)
{
}

RerankError RerankService::ValidateRequest(const RerankRequest &request) const
{
    if (request.model.empty())
    {
        return {
            RerankErrorKind::InvalidRequest,
            "model_required",
            "model is required"};
    }

    if (request.query.empty())
    {
        return {
            RerankErrorKind::InvalidRequest,
            "invalid_query",
            "query must be a non-empty string"};
    }

    if (request.documents.empty())
    {
        return {
            RerankErrorKind::InvalidRequest,
            "invalid_documents",
            "documents must be a non-empty array of strings"};
    }

    if (request.top_n < 0)
    {
        return {
            RerankErrorKind::InvalidRequest,
            "invalid_top_n",
            "top_n must be greater than or equal to 0"};
    }

    if (!model_catalog_service_.HasModel(request.model))
    {
        return {
            RerankErrorKind::InvalidRequest,
            "model_not_found",
            "model not found: " + request.model};
    }

    if (!model_catalog_service_.SupportsCapability(request.model, request.capability, request.inference_backend))
    {
        return {
            RerankErrorKind::InvalidRequest,
            "capability_not_supported",
            "model does not support capability: " + std::string(ToString(request.capability))};
    }

    return {};
}

RerankService::Result RerankService::Run(const RerankRequest &request) const
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
            RerankErrorKind::ServiceUnavailable,
            "backend_not_available",
            "no backend is available for capability: " + std::string(ToString(request.capability))};
        return result;
    }

    auto engine = EngineFactory::Create(request.model, spec.backend);
    if (!engine)
    {
        result.error = {
            RerankErrorKind::ServiceUnavailable,
            "backend_not_available",
            "failed to create backend engine for rerank"};
        return result;
    }

    RerankEngineFacade facade(engine);
    if (!facade.Supports(request.capability))
    {
        result.error = {
            RerankErrorKind::ServiceUnavailable,
            "backend_not_available",
            "resolved backend does not implement rerank"};
        return result;
    }

    result.response.model = spec.model_id.empty() ? request.model : spec.model_id;
    if (!facade.RunRerank(request, result.response, result.response.error_message))
    {
        result.error = {
            RerankErrorKind::Internal,
            "backend_execution_failed",
            result.response.error_message.empty() ? "rerank execution failed" : result.response.error_message};
        return result;
    }

    return result;
}
