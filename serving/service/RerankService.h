#pragma once

#include "serving/service/RerankTypes.h"

class ModelCatalogService;

class RerankService
{
public:
    struct Result
    {
        RerankResponse response;
        RerankError error;
    };

    explicit RerankService(ModelCatalogService &model_catalog_service);

    RerankError ValidateRequest(const RerankRequest &request) const;
    Result Run(const RerankRequest &request) const;

private:
    ModelCatalogService &model_catalog_service_;
};
