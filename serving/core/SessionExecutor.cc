#include "serving/core/SessionExecutor.h"
#include "glog/logging.h"

#include <cstdlib>

bool SessionExecutor::Submit(const std::shared_ptr<Session> &session, std::function<void()> task)
{
    if (!session)
        return false;

    auto get_max_pending = []() -> size_t {
        const char *v = std::getenv("MAX_SESSION_PENDING");
        if (!v || !*v)
            return Session::kMaxPending;
        try
        {
            int n = std::stoi(v);
            return n > 0 ? static_cast<size_t>(n) : Session::kMaxPending;
        }
        catch (...)
        {
            return Session::kMaxPending;
        }
    };

    const size_t max_pending = get_max_pending();
    bool need_schedule = false;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        if (session->pending.size() >= max_pending)
        {
            LOG(WARNING) << "[SessionExecutor] queue full session=" << session->session_id
                         << " size=" << session->pending.size();
            return false;
        }
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
    return true;
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
