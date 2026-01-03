#include "HttpStreamSession.h"
#include "http_types.h"

#include <glog/logging.h>
#include <sstream>

HttpStreamSession::HttpStreamSession(const std::string &request_id, std::shared_ptr<HttpResponse> response)
                                    : request_id_(request_id), response_(std::move(response)) 
{
}

HttpStreamSession::~HttpStreamSession()
{
    Close();
}

void HttpStreamSession::Start()
{
    self_ = shared_from_this();
    LOG(INFO) << "[session] Start() request_id=" << request_id_;

    // SSE headers
    response_->SetHeader("Content-Type", "text/event-stream");
    response_->SetHeader("Cache-Control", "no-cache");
    response_->SetHeader("Connection", "keep-alive");
}

bool HttpStreamSession::IsAlive() const
{
    return !closed_.load();
}

void HttpStreamSession::Write(const std::string &data)
{
    if (!IsAlive())
        return;

    response_->Write(data);
}

void HttpStreamSession::Close()
{
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true))
        return;

    self_.reset();
}
