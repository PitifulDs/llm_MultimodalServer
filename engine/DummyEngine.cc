#include "DummyEngine.h"
#include "serving/core/ServingContext.h"

#include <glog/logging.h>
#include <thread>
#include <chrono>

void DummyEngine::Run(std::shared_ptr<ServingContext> ctx)
{
    if(!ctx){
        return;
    }

    LOG(INFO) << "[dummy] start req=" << ctx->request_id;

    for (int i = 0; i < 20; ++i)
    {
        // 核心：支持取消
        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            LOG(INFO) << "[dummy] cancelled req=" << ctx->request_id;
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        // 正确：EmitDelta 传 string
        ctx->EmitDelta("hello ");

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ctx->EmitFinish(FinishReason::stop);
    LOG(INFO) << "[dummy] finished req=" << ctx->request_id;
}
