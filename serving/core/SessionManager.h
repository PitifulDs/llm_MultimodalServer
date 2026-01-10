#pragma once
#include <chrono>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "serving/core/Session.h"

class SessionManager {
public:
    using Clock = Session::Clock;

    struct Options{
        // Session 超时时间：超过这个时间没访问就可回收
        std::chrono::seconds idle_ttl{std::chrono::minutes(30)};

        // 最大session数：超过则按LRU淘汰
        size_t max_sessions{1024};

        // 每次gc最多回收多少个（避免一次gc卡太久）
        size_t gc_batch{64};

        Options()
            : idle_ttl(std::chrono::minutes(30)),
              max_sessions(1024),
              gc_batch(64) {}
    };

    explicit SessionManager(const Options &op);

    // 获取或创建（不存在则创建）
    std::shared_ptr<Session> getOrCreate(const std::string &session_id, const std::string &model);
    
    // 只获取（不存在返回空）
    std::shared_ptr<Session> get(const std::string &session_id);

    // 显式关闭并移除
    bool close(const std::string &session_id);

    // 主动触碰（刷新 last_active + LRU）
    void touch(const std::string &session_id);

    // 垃圾回收：超时 / closed / LRU
    size_t gc();

    // 统计信息
    size_t size() const;

private:
    // LRU: list front = most recent, back = least recent
    using LruList = std::list<std::string>;

    struct Entry
    {
        std::shared_ptr<Session> session;
        LruList::iterator lru_it;
    };

private:
    void moveToFront_(Entry &e);
    bool shouldExpire_(const Session &s, Clock::time_point now) const;
    size_t evictIfNeeded_(Clock::time_point now);
    bool eraseUnlocked_(const std::string &session_id);

private:
    Options opt_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> map_;
    LruList lru_; // only store session_id
};




