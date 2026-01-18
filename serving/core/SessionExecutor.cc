#include "serving/core/SessionExecutor.h"
#include "glog/logging.h"

void SessionExecutor::Submit(const std::shared_ptr<Session> &session, std::function<void()> task)
{
    if (!session)
        return;

    bool need_schedule = false;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        session->pending.push_back(std::move(task));
        if (!session->running)
        {
            session->running = true;
            need_schedule = true;
        }
    }

    if (need_schedule)
    {
        // 串行 drain：同 session 同时只会有一个 drain 在跑
        pool_.Submit([this, session]{
            Drain(session); 
        });
    }
}

void SessionExecutor::Drain(const std::shared_ptr<Session> &session)
{
    while (true)
    {
        std::function<void()> task;

        {
            std::lock_guard<std::mutex> lk(session->mu);
            if (session->pending.empty())
            {
                session->running = false;
                return;
            }
            task = std::move(session->pending.front());
            session->pending.pop_front();
        }

        // 执行任务（锁外）
        try
        {
            task();
        }
        catch (...)
        {
            LOG(ERROR) << "[SessionExecutor] task threw exception, session=" << session->session_id;
        }
    }
}
