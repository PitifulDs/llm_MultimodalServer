#pragma once

#include <string>
#include <atomic>
#include <memory>

class HttpResponse;

class HttpStreamSession : public std::enable_shared_from_this<HttpStreamSession>
{
public:
    HttpStreamSession(const std::string &request_id, std::shared_ptr<HttpResponse> response);
    ~HttpStreamSession();

    void Start();
    void Close();
    bool IsAlive() const;

    // 唯一对外能力
    void Write(const std::string &data);

private:
    std::string request_id_;
    std::shared_ptr<HttpResponse> response_;
    std::shared_ptr<HttpStreamSession> self_;
    std::atomic<bool> closed_{false};
};
