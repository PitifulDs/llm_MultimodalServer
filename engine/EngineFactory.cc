#include "engine/EngineFactory.h"
#include "engine/DummyEngine.h"
#include "engine/LlamaEngine.h"
#include "serving/core/ModelEngine.h" // 返回 ModelEngine
#include <memory>
#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex g_mu;
    std::unordered_map<std::string, std::shared_ptr<ModelEngine>> g_cache; 

    // 真正的构造逻辑（不带缓存）
    std::shared_ptr<ModelEngine> CreateNewEngine(const std::string &model)
    {
        if (model == "llama")
        {
            return std::make_shared<LlamaEngine>("/home/dongsong/workspace/llm_MultimodalServer/llm_MultimodalServer/models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf");
        }
        if (model == "dummy")
        {
            return std::make_shared<DummyEngine>("Hello");
        }
        // 其它模型...
        return nullptr;
    }
} // namespace
std::shared_ptr<ModelEngine> EngineFactory::Create(const std::string &model)
{
    { // 先查缓存
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_cache.find(model);
        if (it != g_cache.end())
            return it->second;
    }

    // 锁外创建：避免加载模型时一直占着锁（可选但建议）
    auto eng = CreateNewEngine(model);
    if (!eng)
        return nullptr;

    // 二次检查 + 写入（防止并发下重复创建）
    std::lock_guard<std::mutex> lk(g_mu);
    auto &slot = g_cache[model];
    if (!slot)
        slot = eng;
    return slot;
}

void EngineFactory::ClearCache()
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_cache.clear();
}
