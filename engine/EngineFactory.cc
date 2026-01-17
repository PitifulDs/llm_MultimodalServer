#include "engine/EngineFactory.h"
#include "engine/DummyEngine.h"
#include "engine/LlamaEngine.h"
#include "serving/core/ModelEngine.h" // 返回 ModelEngine
#include <memory>

std::shared_ptr<ModelEngine> EngineFactory::Create(const std::string &model)
{
    if(model == "llama")
    {
        return std::make_shared<LlamaEngine>(
            "/home/dongsong/workspace/llm_MultimodalServer/llm_MultimodalServer/models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf");
    }

    return std::make_shared<DummyEngine>("Hello");
}
