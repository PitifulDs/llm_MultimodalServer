#pragma once

#include <string>
#include <vector>

#include "serving/core/ServingContext.h"

class RedisSessionStore
{
public:
    struct Options
    {
        std::string host{"127.0.0.1"};
        int port{6379};
        int db{0};
        std::string key_prefix{"edge:session:"};
        int ttl_seconds{1800};
        int timeout_ms{1000};
    };

    explicit RedisSessionStore(Options opt);

    bool LoadHistory(const std::string &session_id, std::vector<Message> &history_out);
    bool SaveHistory(const std::string &session_id, const std::vector<Message> &history);
    bool DeleteHistory(const std::string &session_id);

private:
    int Connect() const;
    bool SelectDbIfNeeded(int fd) const;
    bool SendCommand(int fd, const std::vector<std::string> &parts) const;

    bool ReadReplySimpleOk(int fd) const;
    bool ReadReplyInteger(int fd, long long &value) const;
    bool ReadReplyBulkString(int fd, std::string &value, bool &is_nil) const;

    bool ReadLine(int fd, std::string &line) const;
    bool ReadExact(int fd, size_t n, std::string &out) const;
    bool WriteAll(int fd, const std::string &data) const;
    std::string BuildKey(const std::string &session_id) const;

private:
    Options opt_;
};

