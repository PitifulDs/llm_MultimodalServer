#include "serving/core/SessionManager.h"

#include <cstdlib>

#include <glog/logging.h>

#include "serving/core/RedisSessionStore.h"

namespace
{
bool env_true(const char *name, bool def = false)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        return def;
    const std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "on" || s == "ON";
}

int env_int(const char *name, int def)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        return def;
    try
    {
        return std::stoi(v);
    }
    catch (...)
    {
        return def;
    }
}

std::string env_str(const char *name, const char *def)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        return std::string(def);
    return std::string(v);
}
} // namespace

SessionManager::SessionManager(const Options &op)
    : opt_(op)
{
    if (env_true("SESSION_PERSIST_REDIS", false))
    {
        RedisSessionStore::Options ro;
        ro.host = env_str("REDIS_HOST", "127.0.0.1");
        ro.port = env_int("REDIS_PORT", 6379);
        ro.db = env_int("REDIS_DB", 0);
        ro.key_prefix = env_str("SESSION_REDIS_PREFIX", "edge:session:");
        ro.ttl_seconds = env_int("SESSION_REDIS_TTL_SECONDS", static_cast<int>(opt_.idle_ttl.count()));
        ro.timeout_ms = env_int("REDIS_TIMEOUT_MS", 1000);
        redis_store_ = std::make_unique<RedisSessionStore>(std::move(ro));
        LOG(INFO) << "[session] redis persistence enabled host="
                  << env_str("REDIS_HOST", "127.0.0.1")
                  << " port=" << env_int("REDIS_PORT", 6379)
                  << " db=" << env_int("REDIS_DB", 0);
    }
}

SessionManager::~SessionManager() = default;

// ======================== public APIs ========================

std::shared_ptr<Session> SessionManager::getOrCreate(const std::string &session_id,
                                                     const std::string &model,
                                                     const std::string &backend)
{
    std::lock_guard<std::mutex> lk(mu_);

    auto it = map_.find(session_id);
    if (it != map_.end())
    {
        if (it->second.session->model != model || it->second.session->inference_backend != backend)
        {
            LOG(INFO) << "[session] reset sid=" << session_id
                      << " model=" << it->second.session->model << "->" << model
                      << " backend=" << it->second.session->inference_backend << "->" << backend;
            it->second.session->model = model;
            it->second.session->inference_backend = backend;
            it->second.session->history.clear();
            it->second.session->model_ctx.reset();
            it->second.session->closed = false;
        }
        it->second.session->touch();
        moveToFront_(it->second);
        return it->second.session;
    }

    // create new session
    auto s = std::make_shared<Session>(session_id, model, backend);
    if (redis_store_)
    {
        std::vector<Message> persisted;
        if (redis_store_->LoadHistory(session_id, persisted) && !persisted.empty())
        {
            s->history = std::move(persisted);
            LOG(INFO) << "[session] restored history from redis sid=" << session_id
                      << " turns=" << s->history.size();
        }
    }

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
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        removed = eraseUnlocked_(session_id);
    }
    if (removed)
    {
        DeletePersistedHistory(session_id);
    }
    return removed;
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

void SessionManager::PersistHistory(const std::string &session_id, const std::vector<Message> &history)
{
    if (!redis_store_)
        return;
    if (session_id.empty())
        return;
    redis_store_->SaveHistory(session_id, history);
}

void SessionManager::DeletePersistedHistory(const std::string &session_id)
{
    if (!redis_store_)
        return;
    if (session_id.empty())
        return;
    redis_store_->DeleteHistory(session_id);
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
