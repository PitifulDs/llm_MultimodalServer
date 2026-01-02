#include "engine/EngineFactory.h"
#include "engine/DummyEngine.h"
#include <memory>

std::shared_ptr<LLMEngine> EngineFactory::Create(const std::string &model)
{
    // Phase 4.1：先只支持 dummy
    // model 参数先留着，后面接 llama.cpp 用
    (void)model;
    return std::make_shared<DummyEngine>("Hello");
}