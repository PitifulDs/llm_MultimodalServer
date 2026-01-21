#pragma once
#include <functional>
#include <memory>

#include "serving/core/Session.h"
#include "serving/core/ThreadPool.h"

class SessionExecutor
{
public:
    explicit SessionExecutor(ThreadPool &pool) : pool_(pool) {}

    // 提交一个 session 任务（同 session 串行）
    bool Submit(const std::shared_ptr<Session> &session, std::function<void()> task);

private:
    void Drain(const std::shared_ptr<Session> &session);

private:
    ThreadPool& pool_;
};
