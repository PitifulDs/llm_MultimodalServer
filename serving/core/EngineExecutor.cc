#include "serving/core/EngineExecutor.h"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include <glog/logging.h>

#include "engine/EngineFactory.h"
#include "serving/core/ModelEngine.h"
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

    // 仅在锁内：尝试取出一个可运行的 ctx（并把 running=true）
    std::shared_ptr<ServingContext> PopNextLocked(const std::string &model)
    {
        auto &mq = g_queues[model];
        if (mq.running || mq.q.empty())
            return nullptr;

        mq.running = true;
        auto ctx = mq.q.front();
        mq.q.pop_front();
        return ctx;
    }

    // 锁外执行：真正跑模型
    void RunOne(std::shared_ptr<ServingContext> ctx);

    void FinishAndMaybeRunNext(const std::string &model)
    {
        std::shared_ptr<ServingContext> next;

        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_queues.find(model);
            if (it == g_queues.end())
            {
                LOG(INFO) << "[execQ] finish model=" << model << " (queue not found)";
                return;
            }

            auto &mq = it->second;
            mq.running = false;

            LOG(INFO) << "[execQ] finish model=" << model << " q_left=" << mq.q.size();

            next = PopNextLocked(model);
        }

        if (next)
        {
            RunOne(std::move(next));
        }
    }

    void RunOne(std::shared_ptr<ServingContext> ctx)
    {
        if (!ctx)
            return;

        const std::string model = ctx->model;

        {
            std::lock_guard<std::mutex> lk(g_mu);
            // 这里不改队列，只打日志（队列状态已在 PopNextLocked 里改完）
            auto it = g_queues.find(model);
            size_t left = (it == g_queues.end()) ? 0 : it->second.q.size();

            LOG(INFO) << "[execQ] start model=" << model
                      << " req=" << ctx->request_id
                      << " stream=" << ctx->stream
                      << " q_left=" << left;
        }

        // 完成回调：只推进队列，不要在锁内跑模型
        ctx->on_finish = [model](FinishReason)
        {
            FinishAndMaybeRunNext(model);
        };

        auto engine = EngineFactory::Create(ctx->model);
        if (!engine)
        {
            ctx->error_message = "EngineFactory::Create failed, model=" + ctx->model;
            ctx->EmitFinish(FinishReason::error); // 会触发 on_finish -> 推进队列
            return;
        }

        engine->Run(std::move(ctx)); // 同步/异步都行；同步也不会拿着 g_mu
    }

} // namespace

void EngineExecutor::Execute(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx)
        return;

    std::shared_ptr<ServingContext> to_run;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto &mq = g_queues[ctx->model];
        mq.q.push_back(ctx);

        LOG(INFO) << "[execQ] enqueue model=" << ctx->model
                  << " req=" << ctx->request_id
                  << " q=" << mq.q.size()
                  << " running=" << mq.running;

        to_run = PopNextLocked(ctx->model);
    }

    if (to_run)
    {
        RunOne(std::move(to_run));
    }
}
