#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "serving/core/ModelEngine.h"

class StackFlowEngine final : public ModelEngine
{
public:
    struct Options
    {
        std::string host{"127.0.0.1"};
        int port{10001};
        std::string unit_name{"llm"};
        std::string response_format{"llm.utf-8"};
        std::string response_format_stream{"llm.utf-8.stream"};
        int timeout_ms{10000};
        int infer_timeout_ms{0};
        bool reuse_work_id{true};
        bool serialize_reuse{true};
    };

    StackFlowEngine();
    explicit StackFlowEngine(const Options &options);
    void Run(std::shared_ptr<ServingContext> ctx) override;

private:
    std::string host_;
    int port_{10001};
    std::string unit_name_;
    std::string response_format_;
    std::string response_format_stream_;
    int timeout_ms_{10000};
    int infer_timeout_ms_{0};
    bool reuse_work_id_{true};
    bool serialize_reuse_{true};
    std::mutex work_mu_;
    std::string cached_work_id_stream_;
    std::string cached_work_id_nostream_;
    std::string cached_setup_key_stream_;
    std::string cached_setup_key_nostream_;
    static std::mutex reuse_mu_;

    static std::string BuildPrompt(const std::vector<Message> &messages);
    static std::string ExtractSystemPrompt(const std::vector<Message> &messages);
    static int ConnectTcp(const std::string &host, int port);
    static bool SendLine(int fd, const std::string &line);
    static bool ReadLine(int fd, std::string &line, std::string &buffer,
                         const std::atomic<bool> &cancelled, int timeout_ms);
};
