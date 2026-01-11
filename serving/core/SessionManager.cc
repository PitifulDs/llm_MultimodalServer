#include "serving/core/SessionManager.h"

#include <glog/logging.h>

SessionManager::SessionManager(const Options &op)
    : opt_(op)
{
}

// ======================== public APIs ========================

std::shared_ptr<Session> SessionManager::getOrCreate(const std::string &session_id, const std::string &model)
{
    std::lock_guard<std::mutex> lk(mu_);

    auto it = map_.find(session_id);
    if (it != map_.end())
    {
        it->second.session->touch();
        moveToFront_(it->second);
        return it->second.session;
    }

    // create new session
    auto s = std::make_shared<Session>(session_id, model);

    lru_.push_front(session_id);
    Entry e;
    e.session = s;
    e.lru_it = lru_.begin();
    map_.emplace(session_id, std::move(e));

    // 超过上限，触发 LRU 回收
    evictIfNeeded_(Clock::now());

    return s;
}

std::shared_ptr<Session> SessionManager::get(const std::string &session_id)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(session_id);
    if (it == map_.end())
    {
        return nullptr;
    }

    it->second.session->touch();
    moveToFront_(it->second);
    return it->second.session;
}

bool SessionManager::close(const std::string &session_id)
{
    std::lock_guard<std::mutex> lk(mu_);
    return eraseUnlocked_(session_id);
}

void SessionManager::touch(const std::string &session_id)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(session_id);
    if (it == map_.end())
    {
        return;
    }

    it->second.session->touch();
    moveToFront_(it->second);
}

size_t SessionManager::gc()
{
    std::lock_guard<std::mutex> lk(mu_);

    const auto now = Clock::now();
    size_t removed = 0;

    // 从 LRU 最老的开始回收
    auto it = lru_.rbegin();
    while (it != lru_.rend() && removed < opt_.gc_batch)
    {
        const std::string &sid = *it;
        auto mit = map_.find(sid);
        if (mit == map_.end())
        {
            ++it;
            continue;
        }

        const auto &s = *mit->second.session;
        if (shouldExpire_(s, now) || s.closed)
        {
            LOG(INFO) << "[session-gc] remove session=" << sid;
            it++; // rbegin 擦除前先走一步
            eraseUnlocked_(sid);
            removed++;
        }
        else
        {
            // LRU 是按时间排序的，前面的都更新鲜，可以直接 break
            break;
        }
    }

    return removed;
}

size_t SessionManager::size() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

// ======================== private helpers ========================

void SessionManager::moveToFront_(Entry &e)
{
    lru_.erase(e.lru_it);
    lru_.push_front(e.session->session_id);
    e.lru_it = lru_.begin();
}

bool SessionManager::shouldExpire_(const Session &s,
                                   Clock::time_point now) const
{
    return (now - s.last_active) > opt_.idle_ttl;
}

size_t SessionManager::evictIfNeeded_(Clock::time_point now)
{
    size_t removed = 0;
    while (map_.size() > opt_.max_sessions && !lru_.empty())
    {
        const std::string &sid = lru_.back();
        LOG(INFO) << "[session-gc] evict LRU session=" << sid;
        eraseUnlocked_(sid);
        removed++;
    }
    return removed;
}

bool SessionManager::eraseUnlocked_(const std::string &session_id)
{
    auto it = map_.find(session_id);
    if (it == map_.end())
    {
        return false;
    }

    lru_.erase(it->second.lru_it);
    map_.erase(it); // shared_ptr 析构 → ModelContext 析构 → KV cache free
    return true;
}
