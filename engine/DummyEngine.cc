#include "engine/DummyEngine.h"

// ⭐ Step 4.5：统一 Run(ctx)
void DummyEngine::Run(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx)
        return;

    // 简单模拟生成
    ctx->EmitDelta(text_);

    LOG(INFO) << "[dummy] before EmitFinish req=" << ctx->request_id;
    ctx->EmitFinish(FinishReason::stop);
    LOG(INFO) << "[dummy] after EmitFinish req=" << ctx->request_id;
}
