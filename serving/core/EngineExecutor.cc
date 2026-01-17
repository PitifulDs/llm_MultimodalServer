#include "serving/core/EngineExecutor.h"
#include "serving/core/ServingContext.h"
#include "serving/core/ModelEngine.h"
#include "engine/EngineFactory.h"

#include <utility>

// ================= ThreadPool =================

EngineExecutor::ThreadPool::ThreadPool(size_t n_threads)
{
    workers_.reserve(n_threads);
    for (size_t i = 0; i < n_threads; ++i)
    {
        workers_.emplace_back([this]
                              { WorkerLoop(); });
    }
}

EngineExecutor::ThreadPool::~ThreadPool()
{
    stop_.store(true);
    cv_.notify_all();
    for (auto &t : workers_)
    {
        if (t.joinable())
            t.join();
    }
}

void EngineExecutor::ThreadPool::Submit(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void EngineExecutor::ThreadPool::WorkerLoop()
{
    while (!stop_.load())
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&]
                     { return stop_.load() || !q_.empty(); });
            if (stop_.load() && q_.empty())
                return;
            task = std::move(q_.front());
            q_.pop_front();
        }
        task();
    }
}

// ================= EngineExecutor =================

EngineExecutor::EngineExecutor(size_t worker_threads)
    : pool_(worker_threads) {}

EngineExecutor::~EngineExecutor() = default;

void EngineExecutor::Execute(std::shared_ptr<ServingContext> ctx)
{
    // 1) 基础校验
    if (!ctx)
        return;

    // 如果已经 finished，就不再执行
    if (ctx->finished.load())
        return;

    // 2) per-model 串行投递
    const std::string model = ctx->model;

    SubmitPerModel(model, [ctx]{
        // 任务开始时再检查一次取消/结束
        if (ctx->finished.load()) return;
        if (ctx->cancelled.load()) {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        // 统一在 executor 内获取（强缓存：不会每次创建）
        auto engine = EngineFactory::Create(ctx->model);
        if (!engine) {
            ctx->error_message = "EngineExecutor: EngineFactory::Create failed, model=" + ctx->model;
            ctx->EmitFinish(FinishReason::error);
            return;
        }

        // 统一入口（engine 内部自己用 ctx->stream / EmitDelta / EmitFinish）
        engine->Run(ctx);

        // 兜底：如果 engine 忘了 finish，这里补一个 stop（防止 ExecuteAndWait 永久等）
        if (!ctx->finished.load()) {
            ctx->EmitFinish(FinishReason::stop);
        } 
    });
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

void EngineExecutor::SubmitPerModel(const std::string &model,  std::function<void()> task)
{
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
}

void EngineExecutor::RunModelQueue(std::string model,
                                   std::shared_ptr<ModelQueue> mq)
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
