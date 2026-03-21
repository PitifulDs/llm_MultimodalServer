#include "engine/EngineFactory.h"
#include "engine/DummyEngine.h"
#include "engine/LlamaEngine.h"
#include "engine/ModelRegistry.h"
#include "engine/StackFlowEngine.h"
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
        const ModelSpec spec = ModelRegistry::Resolve(model);
        if (!spec.valid)
            return nullptr;

        if (spec.engine == "stackflow")
        {
            StackFlowEngine::Options options;
            options.host = spec.stackflow_host;
            options.port = spec.stackflow_port;
            options.unit_name = spec.stackflow_unit;
            options.response_format = spec.stackflow_response_format;
            options.response_format_stream = spec.stackflow_response_format_stream;
            options.timeout_ms = spec.stackflow_timeout_ms;
            options.infer_timeout_ms = spec.stackflow_infer_timeout_ms;
            options.reuse_work_id = spec.stackflow_reuse_work_id;
            options.serialize_reuse = spec.stackflow_serialize_reuse;
            return std::make_shared<StackFlowEngine>(options);
        }
        if (spec.engine == "llama")
        {
            return std::make_shared<LlamaEngine>(spec.model_path);
        }
        if (spec.engine == "dummy")
        {
            return std::make_shared<DummyEngine>("Hello");
        }
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
