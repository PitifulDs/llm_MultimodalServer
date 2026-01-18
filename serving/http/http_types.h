// http_types.h
#pragma once
#include <string>

struct HttpRequest
{
    std::string body;
    virtual bool HasQuery(const std::string &key) const = 0;
    virtual std::string Query(const std::string &key) const = 0;
    virtual ~HttpRequest() = default;
};

struct HttpResponse
{
    virtual void SetHeader(const std::string &, const std::string &) = 0;
    virtual void Write(const std::string &data) = 0;
    virtual bool IsAlive() const = 0;
    virtual void SetStatus(int code, const std::string &reason = "") = 0;
    virtual void End() = 0; // 非流式写完后统一关闭/flush
    virtual ~HttpResponse() = default;
};
