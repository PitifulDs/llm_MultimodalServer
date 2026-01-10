#include "engine/DummyEngine.h"

// ⭐ Step 4.5：统一 Run(ctx)
void DummyEngine::Run(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx)
        return;

    // 简单模拟生成
    ctx->EmitDelta(text_);

    ctx->EmitFinish(FinishReason::stop);
}
