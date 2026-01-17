#include "serving/core/EngineExecutor.h"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <condition_variable>

#include "glog/logging.h"
#include "engine/EngineFactory.h"
#include "serving/core/ModelEngine.h" // 保证 ModelEngine 完整类型
#include "serving/core/ServingContext.h"

namespace
{

    struct ModelQueue
    {
        bool running = false;
        std::deque<std::shared_ptr<ServingContext>> q;
    };

    std::mutex g_mu;
    std::unordered_map<std::string, ModelQueue> g_queues;

    void StartNextMaybe(const std::string &model);

    void OnFinished(const std::string &model)
    {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_queues.find(model);
            if (it == g_queues.end())
            {
                LOG(INFO) << "[execQ] finish model=" << model << " (queue not found)";
                return;
            }
            it->second.running = false;
            LOG(INFO) << "[execQ] finish model=" << model
                      << " q_left=" << it->second.q.size();
        }
        // 锁外启动下一个，避免死锁
        StartNextMaybe(model);
    }

    void StartNextMaybe(const std::string &model)
    {
        std::shared_ptr<ServingContext> ctx;

        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto &mq = g_queues[model];

            if (mq.running || mq.q.empty())
                return;

            mq.running = true;
            ctx = mq.q.front();
            mq.q.pop_front();

            LOG(INFO) << "[execQ] start model=" << model
                      << " req=" << ctx->request_id
                      << " stream=" << ctx->stream
                      << " q_left=" << mq.q.size();

            // 链式 on_finish：先执行用户的 on_finish，再驱动队列
            auto prev_finish = ctx->on_finish;
            ctx->on_finish = [model, prev_finish](FinishReason r)
            {
                if (prev_finish)
                    prev_finish(r);
                OnFinished(model);
            };
        }

        // ======== 锁外执行 ========
        auto engine = EngineFactory::Create(ctx->model);
        if (!engine)
        {
            ctx->error_message = "EngineFactory::Create failed, model=" + ctx->model;
            ctx->EmitFinish(FinishReason::error); // 会触发 ctx->on_finish -> OnFinished
            return;
        }

        engine->Run(std::move(ctx));
    }

} // namespace

void EngineExecutor::Execute(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx)
        return;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto &mq = g_queues[ctx->model];
        mq.q.push_back(ctx);

        LOG(INFO) << "[execQ] enqueue model=" << ctx->model
                  << " req=" << ctx->request_id
                  << " q=" << mq.q.size()
                  << " running=" << mq.running;
    }

    StartNextMaybe(ctx->model);
}

void EngineExecutor::ExecuteAndWait(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx)
        return;

    struct Waiter
    {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        FinishReason reason = FinishReason::stop;
    };

    auto waiter = std::make_shared<Waiter>();

    // 保留用户原来的 on_finish（比如你未来还要做额外动作）
    auto user_finish = ctx->on_finish;

    // 把等待逻辑挂进去（注意：不会覆盖队列驱动，因为队列驱动是在 StartNextMaybe 里链式包一层）
    ctx->on_finish = [waiter, user_finish](FinishReason r)
    {
        if (user_finish)
            user_finish(r);

        {
            std::lock_guard<std::mutex> lk(waiter->mu);
            waiter->done = true;
            waiter->reason = r;
        }
        waiter->cv.notify_one();
    };

    Execute(ctx);

    std::unique_lock<std::mutex> lk(waiter->mu);
    waiter->cv.wait(lk, [&]
                    { return waiter->done; });
}
