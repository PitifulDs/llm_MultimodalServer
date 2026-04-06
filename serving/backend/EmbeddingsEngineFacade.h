#pragma once

#include <memory>
#include <string>

#include "serving/service/EmbeddingsTypes.h"

class ModelEngine;

class EmbeddingsEngineFacade
{
public:
    explicit EmbeddingsEngineFacade(std::shared_ptr<ModelEngine> engine);

    bool Supports(ModelCapability capability) const;
    bool RunEmbeddings(const EmbeddingsRequest &request,
                       EmbeddingsResponse &response,
                       std::string &error_message) const;

private:
    std::shared_ptr<ModelEngine> engine_;
};
