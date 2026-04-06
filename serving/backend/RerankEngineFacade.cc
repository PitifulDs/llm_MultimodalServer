#include "serving/backend/RerankEngineFacade.h"

#include "engine/LlamaEngine.h"
#include "serving/core/ModelEngine.h"

#include <utility>

RerankEngineFacade::RerankEngineFacade(std::shared_ptr<ModelEngine> engine)
    : engine_(std::move(engine))
{
}

bool RerankEngineFacade::Supports(ModelCapability capability) const
{
    return capability == ModelCapability::Rerank &&
           std::dynamic_pointer_cast<LlamaEngine>(engine_) != nullptr;
}

bool RerankEngineFacade::RunRerank(const RerankRequest &request,
                                   RerankResponse &response,
                                   std::string &error_message) const
{
    auto llama_engine = std::dynamic_pointer_cast<LlamaEngine>(engine_);
    if (!llama_engine)
    {
        error_message = "backend does not implement rerank";
        return false;
    }

    return llama_engine->RunRerank(request, response, error_message);
}
