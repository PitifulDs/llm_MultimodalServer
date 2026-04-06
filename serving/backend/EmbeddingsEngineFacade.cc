#include "serving/backend/EmbeddingsEngineFacade.h"

#include "engine/LlamaEngine.h"
#include "serving/core/ModelEngine.h"

#include <utility>

EmbeddingsEngineFacade::EmbeddingsEngineFacade(std::shared_ptr<ModelEngine> engine)
    : engine_(std::move(engine))
{
}

bool EmbeddingsEngineFacade::Supports(ModelCapability capability) const
{
    return capability == ModelCapability::Embeddings &&
           std::dynamic_pointer_cast<LlamaEngine>(engine_) != nullptr;
}

bool EmbeddingsEngineFacade::RunEmbeddings(const EmbeddingsRequest &request,
                                           EmbeddingsResponse &response,
                                           std::string &error_message) const
{
    auto llama_engine = std::dynamic_pointer_cast<LlamaEngine>(engine_);
    if (!llama_engine)
    {
        error_message = "backend does not implement embeddings";
        return false;
    }

    return llama_engine->RunEmbeddings(request, response, error_message);
}
