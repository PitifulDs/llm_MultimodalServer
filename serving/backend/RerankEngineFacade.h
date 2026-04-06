#pragma once

#include <memory>
#include <string>

#include "serving/service/RerankTypes.h"

class ModelEngine;

class RerankEngineFacade
{
public:
    explicit RerankEngineFacade(std::shared_ptr<ModelEngine> engine);

    bool Supports(ModelCapability capability) const;
    bool RunRerank(const RerankRequest &request,
                   RerankResponse &response,
                   std::string &error_message) const;

private:
    std::shared_ptr<ModelEngine> engine_;
};
