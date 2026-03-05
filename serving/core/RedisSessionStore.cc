#include "serving/core/RedisSessionStore.h"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <glog/logging.h>

#include "utils/json.hpp"

using json = nlohmann::json;

RedisSessionStore::RedisSessionStore(Options opt)
    : opt_(std::move(opt))
{
}

bool RedisSessionStore::LoadHistory(const std::string &session_id, std::vector<Message> &history_out)
{
    const int fd = Connect();
    if (fd < 0)
        return false;

    bool ok = false;
    do
    {
        if (!SelectDbIfNeeded(fd))
            break;
        if (!SendCommand(fd, {"GET", BuildKey(session_id)}))
            break;

        std::string payload;
        bool is_nil = false;
        if (!ReadReplyBulkString(fd, payload, is_nil))
            break;
        if (is_nil)
        {
            ok = true;
            break;
        }

        json arr = json::parse(payload, nullptr, false);
        if (!arr.is_array())
            break;

        std::vector<Message> tmp;
        tmp.reserve(arr.size());
        for (const auto &m : arr)
        {
            if (!m.is_object())
                continue;
            const std::string role = m.value("role", "");
            const std::string content = m.value("content", "");
            if (role.empty())
                continue;
            tmp.push_back({role, content});
        }
        history_out = std::move(tmp);
        ok = true;
    } while (false);

    ::close(fd);
    if (!ok)
    {
        LOG(WARNING) << "[redis-session] load failed sid=" << session_id;
    }
    return ok;
}

bool RedisSessionStore::SaveHistory(const std::string &session_id, const std::vector<Message> &history)
{
    const int fd = Connect();
    if (fd < 0)
        return false;

    bool ok = false;
    do
    {
        if (!SelectDbIfNeeded(fd))
            break;

        json arr = json::array();
        for (const auto &m : history)
        {
            arr.push_back({{"role", m.role}, {"content", m.content}});
        }
        const std::string payload = arr.dump(-1, ' ', false, json::error_handler_t::replace);

        if (!SendCommand(fd, {"SETEX", BuildKey(session_id), std::to_string(opt_.ttl_seconds), payload}))
            break;
        if (!ReadReplySimpleOk(fd))
            break;
        ok = true;
    } while (false);

    ::close(fd);
    if (!ok)
    {
        LOG(WARNING) << "[redis-session] save failed sid=" << session_id;
    }
    return ok;
}

bool RedisSessionStore::DeleteHistory(const std::string &session_id)
{
    const int fd = Connect();
    if (fd < 0)
        return false;

    bool ok = false;
    do
    {
        if (!SelectDbIfNeeded(fd))
            break;
        if (!SendCommand(fd, {"DEL", BuildKey(session_id)}))
            break;
        long long deleted = 0;
        if (!ReadReplyInteger(fd, deleted))
            break;
        (void)deleted;
        ok = true;
    } while (false);

    ::close(fd);
    return ok;
}

int RedisSessionStore::Connect() const
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    const std::string port_str = std::to_string(opt_.port);
    const int rc = ::getaddrinfo(opt_.host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0)
    {
        LOG(WARNING) << "[redis-session] getaddrinfo failed host=" << opt_.host
                     << " port=" << opt_.port
                     << " err=" << gai_strerror(rc);
        return -1;
    }

    int fd = -1;
    for (auto *p = res; p != nullptr; p = p->ai_next)
    {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;

        struct timeval tv;
        tv.tv_sec = opt_.timeout_ms / 1000;
        tv.tv_usec = (opt_.timeout_ms % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);

    if (fd < 0)
    {
        LOG(WARNING) << "[redis-session] connect failed host=" << opt_.host
                     << " port=" << opt_.port
                     << " errno=" << errno;
    }
    return fd;
}

bool RedisSessionStore::SelectDbIfNeeded(int fd) const
{
    if (opt_.db <= 0)
        return true;
    if (!SendCommand(fd, {"SELECT", std::to_string(opt_.db)}))
        return false;
    return ReadReplySimpleOk(fd);
}

bool RedisSessionStore::SendCommand(int fd, const std::vector<std::string> &parts) const
{
    std::string req;
    req.reserve(64);
    req += "*";
    req += std::to_string(parts.size());
    req += "\r\n";
    for (const auto &p : parts)
    {
        req += "$";
        req += std::to_string(p.size());
        req += "\r\n";
        req += p;
        req += "\r\n";
    }
    return WriteAll(fd, req);
}

bool RedisSessionStore::ReadReplySimpleOk(int fd) const
{
    std::string line;
    char type = 0;
    std::string t;
    if (!ReadExact(fd, 1, t))
        return false;
    type = t[0];
    if (!ReadLine(fd, line))
        return false;
    if (type == '+')
        return true;
    if (type == '-')
    {
        LOG(WARNING) << "[redis-session] redis error: " << line;
    }
    return false;
}

bool RedisSessionStore::ReadReplyInteger(int fd, long long &value) const
{
    value = 0;
    std::string t;
    if (!ReadExact(fd, 1, t))
        return false;
    if (t[0] == '-')
    {
        std::string err;
        if (ReadLine(fd, err))
            LOG(WARNING) << "[redis-session] redis error: " << err;
        return false;
    }
    if (t[0] != ':')
        return false;
    std::string line;
    if (!ReadLine(fd, line))
        return false;
    try
    {
        value = std::stoll(line);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

bool RedisSessionStore::ReadReplyBulkString(int fd, std::string &value, bool &is_nil) const
{
    value.clear();
    is_nil = false;

    std::string t;
    if (!ReadExact(fd, 1, t))
        return false;

    if (t[0] == '-')
    {
        std::string err;
        if (ReadLine(fd, err))
            LOG(WARNING) << "[redis-session] redis error: " << err;
        return false;
    }
    if (t[0] != '$')
        return false;

    std::string line;
    if (!ReadLine(fd, line))
        return false;

    long long n = -1;
    try
    {
        n = std::stoll(line);
    }
    catch (...)
    {
        return false;
    }

    if (n == -1)
    {
        is_nil = true;
        return true;
    }
    if (n < 0)
        return false;

    std::string raw;
    if (!ReadExact(fd, static_cast<size_t>(n) + 2, raw))
        return false;
    value.assign(raw.data(), static_cast<size_t>(n));
    return true;
}

bool RedisSessionStore::ReadLine(int fd, std::string &line) const
{
    line.clear();
    char c = 0;
    char prev = 0;
    while (true)
    {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0)
            return false;
        line.push_back(c);
        if (prev == '\r' && c == '\n')
        {
            line.resize(line.size() - 2);
            return true;
        }
        prev = c;
    }
}

bool RedisSessionStore::ReadExact(int fd, size_t n, std::string &out) const
{
    out.clear();
    out.resize(n);
    size_t off = 0;
    while (off < n)
    {
        const ssize_t r = ::recv(fd, &out[off], n - off, 0);
        if (r <= 0)
            return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

bool RedisSessionStore::WriteAll(int fd, const std::string &data) const
{
    size_t off = 0;
    while (off < data.size())
    {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0)
            return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

std::string RedisSessionStore::BuildKey(const std::string &session_id) const
{
    return opt_.key_prefix + session_id;
}

