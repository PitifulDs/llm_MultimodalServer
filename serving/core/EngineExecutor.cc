#include "serving/core/EngineExecutor.h"
#include "serving/core/ServingContext.h"
#include "serving/core/ModelEngine.h"
#include "engine/EngineFactory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <glog/logging.h>
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

    auto get_env_int = [](const char *name, int def) -> int {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return def;
        try
        {
            int n = std::stoi(v);
            return n > 0 ? n : def;
        }
        catch (...)
        {
            return def;
        }
    };

    const int max_queue_wait_ms = get_env_int("MAX_QUEUE_WAIT_MS", 2000);
    const int max_model_queue = get_env_int("MAX_MODEL_QUEUE", 64);

    const auto enqueued_at = std::chrono::steady_clock::now();

    bool ok = SubmitPerModel(model, [this, ctx, enqueued_at, max_queue_wait_ms]
    {
        // 任务开始时再检查一次
        if (ctx->finished.load(std::memory_order_acquire))
            return;

        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        const auto start_at = std::chrono::steady_clock::now();
        const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(start_at - enqueued_at).count();
        if (max_queue_wait_ms > 0 && wait_ms > max_queue_wait_ms)
        {
            ctx->error_message = "EngineExecutor: queue wait timeout";
            ctx->params["error_code"] = "overloaded";
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        LOG(INFO) << "[execQ] start model=" << ctx->model
                  << " req=" << ctx->request_id
                  << " wait_ms=" << wait_ms;

        // 获取/复用 engine（你已有缓存逻辑就保留；这里示例直接 Create）
        std::shared_ptr<ModelEngine> engine;
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto &slot = engines_[ctx->model];
            if (!slot)
                slot = EngineFactory::Create(ctx->model);
            engine = slot;
        }

        if (!engine)
        {
            ctx->error_message = "EngineExecutor: EngineFactory::Create failed, model=" + ctx->model;
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        // 引擎执行（内部会轮询 ctx->cancelled 并 EmitDelta/EmitFinish）
        engine->Run(ctx);

        // 兜底：如果引擎忘了 finish，按 cancelled 优先，否则 stop
        if (!ctx->finished.load(std::memory_order_acquire))
        {
            if (ctx->cancelled.load(std::memory_order_acquire))
                ctx->EmitFinish(FinishReason::cancelled);
            else
                ctx->EmitFinish(FinishReason::stop);
        } 
    }, static_cast<size_t>(max_model_queue));

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

bool EngineExecutor::SubmitPerModel(const std::string &model,  std::function<void()> task, size_t max_queue)
{
    constexpr size_t MAX_QUEUE_FLOOR = 1;

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
        const size_t cap = std::max(MAX_QUEUE_FLOOR, max_queue);
        if (mq->tasks.size() >= cap)
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
