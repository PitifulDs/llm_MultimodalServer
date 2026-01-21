#include "serving/core/EngineExecutor.h"
#include "serving/core/ServingContext.h"
#include "serving/core/ModelEngine.h"
#include "engine/EngineFactory.h"

#include <utility>

// ================= EngineExecutor =================

EngineExecutor::EngineExecutor(ThreadPool &pool)
    : pool_(pool) 
{
}

EngineExecutor::~EngineExecutor() = default;

bool EngineExecutor::Execute(std::shared_ptr<ServingContext> ctx)
{
    // 1) 基础校验
    if (!ctx)
        return false;

    // 如果已经 finished，就不再执行
    if (ctx->finished.load())
        return false;

    // 2) per-model 串行投递
    const std::string model = ctx->model;

    bool ok = SubmitPerModel(model, [this, ctx]
    {
        if (ctx->finished.load()) return;
        if (ctx->cancelled.load()) {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        std::shared_ptr<ModelEngine> engine;
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto &slot = engines_[ctx->model];
            if (!slot)
                slot = EngineFactory::Create(ctx->model);
            engine = slot;
        }

        if (!engine) {
            ctx->error_message = "EngineExecutor: EngineFactory::Create failed, model=" + ctx->model;
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        engine->Run(ctx);

        if (!ctx->finished.load()) {
            ctx->EmitFinish(FinishReason::stop);
        } 
    });


    if (!ok)
    {
        // 立即失败：避免客户端挂死超时
        ctx->error_message = "EngineExecutor: model queue full, model=" + model;
        ctx->params["error_code"] = "overloaded";
        ctx->EmitFinish(FinishReason::error);
        return false;
    }

    return true;
}

void EngineExecutor::ExecuteAndWait(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx)
        return;
    if (ctx->finished.load())
        return;

    struct WaitState
    {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
    };
    auto st = std::make_shared<WaitState>();

    // 串起原来的 on_finish
    auto user_on_finish = ctx->on_finish;

    ctx->on_finish = [st, user_on_finish](FinishReason r)
    {
        if (user_on_finish)
            user_on_finish(r);
        {
            std::lock_guard<std::mutex> lk(st->mu);
            st->done = true;
        }
        st->cv.notify_one();
    };

    Execute(ctx);

    std::unique_lock<std::mutex> lk(st->mu);
    st->cv.wait(lk, [&]
                { return st->done; });

    // 恢复（关键：避免 ctx 后续再次 finish 时还调用到等待用的 lambda）
    ctx->on_finish = user_on_finish;
}

bool EngineExecutor::SubmitPerModel(const std::string &model,  std::function<void()> task)
{
    constexpr size_t MAX_QUEUE = 64; // 你可以先用 64/128，后续压测调参

    std::shared_ptr<ModelQueue> mq;
    {
        std::lock_guard<std::mutex> lk(map_mu_);
        auto &slot = queues_[model];
        if (!slot)
            slot = std::make_shared<ModelQueue>();
        mq = slot;
    }

    bool need_schedule = false;
    {
        std::lock_guard<std::mutex> lk(mq->mu);

        // backpressure：队列满直接拒绝
        if (mq->tasks.size() >= MAX_QUEUE)
        {
            return false;
        }

        mq->tasks.push_back(std::move(task));
        if (!mq->running)
        {
            mq->running = true;
            need_schedule = true;
        }
    }

    if (need_schedule)
    {
        pool_.Submit([this, model, mq]
                     { RunModelQueue(model, mq); });
    }

    return true;
}

void EngineExecutor::RunModelQueue(std::string model, std::shared_ptr<ModelQueue> mq)
{
    while (true)
    {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lk(mq->mu);
            if (mq->tasks.empty())
            {
                mq->running = false;
                return;
            }
            task = std::move(mq->tasks.front());
            mq->tasks.pop_front();
        }
        task();
    }
}
