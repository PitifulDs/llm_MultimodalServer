#pragma once

#include "serving/service/EmbeddingsTypes.h"

class ModelCatalogService;

class EmbeddingsService
{
public:
    struct Result
    {
        EmbeddingsResponse response;
        EmbeddingsError error;
        std::string resolved_backend;
    };

    explicit EmbeddingsService(ModelCatalogService &model_catalog_service);

    EmbeddingsError ValidateRequest(const EmbeddingsRequest &request) const;
    Result Run(const EmbeddingsRequest &request) const;

private:
    ModelCatalogService &model_catalog_service_;
};
