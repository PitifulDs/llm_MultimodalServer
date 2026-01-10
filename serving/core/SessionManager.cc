#include "serving/core/SessionManager.h"

SessionManager::SessionManager(const Options &op) : opt_(op)
{
}

std::shared_ptr<Session> SessionManager::getOrCreate(const std::string &session_id,  const std::string &model)
{
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lk(mu_);

    auto it = map_.find(session_id);
    if (it != map_.end())
    {
        auto &e = it->second;
        if (e.session && !e.session->closed)
        {
            e.session->last_active = now;
            moveToFront_(e);
            return e.session;
        }
        // 如果存在但 closed，直接移除后再创建
        eraseUnlocked_(session_id);
    }

    // 创建新 Session
    auto s = std::make_shared<Session>(session_id, model);
    s->last_active = now;

    lru_.push_front(session_id);
    Entry e;
    e.session = s;
    e.lru_it = lru_.begin();
    map_.emplace(session_id, std::move(e));

    // 容量控制（LRU）
    evictIfNeeded_(now);
    return s;
}

std::shared_ptr<Session> SessionManager::get(const std::string &session_id)
{
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(session_id);
    if (it == map_.end())
        return nullptr;

    auto &e = it->second;
    if (!e.session || e.session->closed)
        return nullptr;

    e.session->last_active = now;
    moveToFront_(e);
    return e.session;
}

void SessionManager::touch(const std::string &session_id)
{
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(session_id);
    if (it == map_.end())
        return;

    auto &e = it->second;
    if (!e.session || e.session->closed)
        return;

    e.session->last_active = now;
    moveToFront_(e);
}

bool SessionManager::close(const std::string &session_id)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(session_id);
    if (it == map_.end())
        return false;

    if (it->second.session)
    {
        it->second.session->closed = true;
    }
    return eraseUnlocked_(session_id);
}

size_t SessionManager::gc()
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lk(mu_);

    size_t freed = 0;

    // 1) 先按 TTL / closed 从 LRU 尾部开始扫（最老的最可能过期）
    for (auto it = lru_.rbegin(); it != lru_.rend() && freed < opt_.gc_batch;)
    {
        const std::string &sid = *it;
        auto mapIt = map_.find(sid);

        // 先把 rbegin 转成可 erase 的 iterator
        auto baseIt = it.base();
        --baseIt; // 指向当前元素（list 的正向迭代器）

        bool erase = false;
        if (mapIt == map_.end() || !mapIt->second.session)
        {
            erase = true;
        }
        else
        {
            const auto &s = *mapIt->second.session;
            if (s.closed || shouldExpire_(s, now))
            {
                erase = true;
            }
        }

        if (erase)
        {
            // eraseUnlocked_ 会同时删除 map_ 和 lru_
            eraseUnlocked_(sid);
            freed++;
            // rbegin 失效，需要重新定位：从 baseIt 的前一个继续
            it = std::reverse_iterator<LruList::iterator>(baseIt);
        }
        else
        {
            ++it;
        }
    }

    // 2) 再做容量控制（LRU）
    freed += evictIfNeeded_(now);
    return freed;
}

size_t SessionManager::size() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

void SessionManager::moveToFront_(Entry &e)
{
    // 已在 front 则不动
    if (e.lru_it == lru_.begin())
        return;

    lru_.erase(e.lru_it);
    lru_.push_front(e.session->session_id);
    e.lru_it = lru_.begin();
}

bool SessionManager::shouldExpire_(const Session &s, Clock::time_point now) const
{
    return (now - s.last_active) > opt_.idle_ttl;
}

size_t SessionManager::evictIfNeeded_(Clock::time_point /*now*/)
{
    size_t freed = 0;

    while (map_.size() > opt_.max_sessions && !lru_.empty())
    {
        const std::string sid = lru_.back();
        if (eraseUnlocked_(sid))
        {
            freed++;
        }
        else
        {
            // 理论上不会发生：兜底避免死循环
            lru_.pop_back();
        }
    }
    return freed;
}

bool SessionManager::eraseUnlocked_(const std::string &session_id)
{
    auto it = map_.find(session_id);
    if (it == map_.end())
    {
        // lru_ 里可能还有残留，尽量清理
        for (auto lit = lru_.begin(); lit != lru_.end(); ++lit)
        {
            if (*lit == session_id)
            {
                lru_.erase(lit);
                break;
            }
        }
        return false;
    }

    // 先删 lru
    lru_.erase(it->second.lru_it);

    // 再删 map（shared_ptr 出作用域时释放 session / model_ctx）
    map_.erase(it);
    return true;
}
