#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "serving/core/ModelEngine.h"

class StackFlowEngine final : public ModelEngine
{
public:
    StackFlowEngine();
    void Run(std::shared_ptr<ServingContext> ctx) override;

private:
    std::string host_;
    int port_{10001};
    std::string unit_name_;
    std::string response_format_;
    std::string response_format_stream_;
    int timeout_ms_{10000};

    static std::string BuildPrompt(const std::vector<Message> &messages);
    static std::string ExtractSystemPrompt(const std::vector<Message> &messages);
    static int ConnectTcp(const std::string &host, int port);
    static bool SendLine(int fd, const std::string &line);
    static bool ReadLine(int fd, std::string &line, std::string &buffer,
                         const std::atomic<bool> &cancelled, int timeout_ms);
};
