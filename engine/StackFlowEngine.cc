#include "engine/StackFlowEngine.h"

#include "utils/json.hpp"
#include <glog/logging.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

using json = nlohmann::json;

namespace
{
    std::string get_env_string(const char *name, const char *def_val)
    {
        const char *v = std::getenv(name);
        return (v && *v) ? std::string(v) : std::string(def_val);
    }

    int get_env_int(const char *name, int def_val)
    {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return def_val;
        try
        {
            int n = std::stoi(v);
            return n > 0 ? n : def_val;
        }
        catch (...)
        {
            return def_val;
        }
    }
} // namespace

StackFlowEngine::StackFlowEngine()
{
    host_ = get_env_string("STACKFLOW_HOST", "127.0.0.1");
    port_ = get_env_int("STACKFLOW_PORT", 10001);
    unit_name_ = get_env_string("STACKFLOW_UNIT", "llm");
    response_format_ = get_env_string("STACKFLOW_RESPONSE_FORMAT", "llm.utf-8");
    response_format_stream_ = get_env_string("STACKFLOW_RESPONSE_FORMAT_STREAM", "llm.utf-8.stream");
    timeout_ms_ = get_env_int("STACKFLOW_TIMEOUT_MS", 10000);
}

std::string StackFlowEngine::BuildPrompt(const std::vector<Message> &messages)
{
    std::ostringstream oss;
    bool first = true;
    for (const auto &m : messages)
    {
        if (!m.content.empty())
        {
            if (!first)
                oss << ' ';
            oss << m.content;
            first = false;
        }
    }
    return oss.str();
}

std::string StackFlowEngine::ExtractSystemPrompt(const std::vector<Message> &messages)
{
    std::ostringstream oss;
    for (const auto &m : messages)
    {
        if (m.role == "system" && !m.content.empty())
        {
            if (!oss.str().empty())
                oss << "\n";
            oss << m.content;
        }
    }
    return oss.str();
}

int StackFlowEngine::ConnectTcp(const std::string &host, int port)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
    {
        return -1;
    }

    int fd = -1;
    for (auto p = res; p != nullptr; p = p->ai_next)
    {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0)
        {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool StackFlowEngine::SendLine(int fd, const std::string &line)
{
    std::string payload = line;
    if (payload.empty() || payload.back() != '\n')
        payload.push_back('\n');

    const char *data = payload.data();
    size_t total = 0;
    while (total < payload.size())
    {
        ssize_t n = ::send(fd, data + total, payload.size() - total, 0);
        if (n <= 0)
            return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool StackFlowEngine::ReadLine(int fd, std::string &line, std::string &buffer,
                               const std::atomic<bool> &cancelled, int timeout_ms)
{
    const int total_timeout_ms = timeout_ms > 0 ? timeout_ms : 10000;
    int waited_ms = 0;
    while (true)
    {
        auto pos = buffer.find('\n');
        if (pos != std::string::npos)
        {
            line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            return true;
        }

        if (cancelled.load(std::memory_order_acquire))
            return false;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int step_ms = 200;
        int ret = ::poll(&pfd, 1, step_ms);
        if (ret < 0)
            return false;
        if (ret == 0)
        {
            waited_ms += step_ms;
            if (waited_ms >= total_timeout_ms)
                return false;
            continue;
        }
        if (!(pfd.revents & POLLIN))
            continue;

        char tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0)
            return false;
        buffer.append(tmp, static_cast<size_t>(n));
    }
}

void StackFlowEngine::Run(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx)
        return;

    const std::string prompt = ctx->is_chat ? BuildPrompt(ctx->messages) : ctx->prompt;
    const std::string system_prompt = ctx->is_chat ? ExtractSystemPrompt(ctx->messages) : "";

    int fd = ConnectTcp(host_, port_);
    if (fd < 0)
    {
        ctx->error_message = "StackFlowEngine: connect failed host=" + host_ + " port=" + std::to_string(port_);
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    std::string buffer;
    std::string line;
    std::string work_id;

    const std::string response_format = ctx->stream ? response_format_stream_ : response_format_;

    int max_tokens = 512;
    auto it = ctx->params.find("max_tokens");
    if (it != ctx->params.end())
    {
        try
        {
            int v = std::stoi(it->second);
            if (v > 0)
                max_tokens = v;
        }
        catch (...)
        {
        }
    }

    json setup_data = {
        {"model", ctx->model.empty() ? unit_name_ : ctx->model},
        {"response_format", response_format},
        {"input", response_format},
        {"enoutput", true},
        {"max_token_len", max_tokens},
        {"prompt", system_prompt}
    };

    json setup_req = {
        {"request_id", ctx->request_id},
        {"work_id", unit_name_},
        {"action", "setup"},
        {"object", "llm.setup"},
        {"data", setup_data}
    };

    if (!SendLine(fd, setup_req.dump()))
    {
        ::close(fd);
        ctx->error_message = "StackFlowEngine: send setup failed";
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    if (!ReadLine(fd, line, buffer, ctx->cancelled, timeout_ms_))
    {
        ::close(fd);
        ctx->error_message = "StackFlowEngine: setup timeout or cancelled";
        ctx->EmitFinish(ctx->cancelled ? FinishReason::cancelled : FinishReason::error);
        return;
    }

    try
    {
        json resp = json::parse(line);
        if (resp.contains("error") && resp["error"].is_object())
        {
            int code = resp["error"].value("code", 0);
            if (code != 0)
            {
                ctx->error_message = resp["error"].value("message", "stackflow error");
                ctx->EmitFinish(FinishReason::error);
                ::close(fd);
                return;
            }
        }
        if (resp.contains("work_id") && resp["work_id"].is_string())
            work_id = resp["work_id"].get<std::string>();
    }
    catch (...)
    {
        ctx->error_message = "StackFlowEngine: setup response parse failed";
        ctx->EmitFinish(FinishReason::error);
        ::close(fd);
        return;
    }

    if (work_id.empty())
    {
        ctx->error_message = "StackFlowEngine: empty work_id";
        ctx->EmitFinish(FinishReason::error);
        ::close(fd);
        return;
    }

    json infer_req;
    infer_req["request_id"] = ctx->request_id;
    infer_req["work_id"] = work_id;
    infer_req["action"] = "inference";
    infer_req["object"] = response_format;
    if (ctx->stream)
    {
        infer_req["data"] = {{"delta", prompt}, {"index", 0}, {"finish", true}};
    }
    else
    {
        infer_req["data"] = prompt;
    }

    if (!SendLine(fd, infer_req.dump()))
    {
        ::close(fd);
        ctx->error_message = "StackFlowEngine: send inference failed";
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    auto send_exit = [&]()
    {
        json exit_req = {
            {"request_id", ctx->request_id},
            {"work_id", work_id},
            {"action", "exit"}
        };
        SendLine(fd, exit_req.dump());
    };

    if (ctx->stream)
    {
        while (!ctx->finished.load(std::memory_order_acquire))
        {
            if (!ReadLine(fd, line, buffer, ctx->cancelled, timeout_ms_))
            {
                if (!ctx->finished.load(std::memory_order_acquire))
                {
                    ctx->EmitFinish(ctx->cancelled ? FinishReason::cancelled : FinishReason::error);
                }
                break;
            }
            try
            {
                json resp = json::parse(line);
                if (resp.contains("error") && resp["error"].is_object())
                {
                    int code = resp["error"].value("code", 0);
                    if (code != 0)
                    {
                        ctx->error_message = resp["error"].value("message", "stackflow error");
                        ctx->EmitFinish(FinishReason::error);
                        break;
                    }
                }

                if (!resp.contains("data"))
                    continue;

                const auto &data = resp["data"];
                if (data.is_object())
                {
                    const std::string delta = data.value("delta", "");
                    const bool finish = data.value("finish", false);
                    if (!delta.empty())
                        ctx->EmitDelta(delta);
                    if (finish)
                    {
                        ctx->EmitFinish(FinishReason::stop);
                        break;
                    }
                }
                else if (data.is_string())
                {
                    ctx->EmitDelta(data.get<std::string>());
                    ctx->EmitFinish(FinishReason::stop);
                    break;
                }
            }
            catch (...)
            {
                ctx->error_message = "StackFlowEngine: response parse failed";
                ctx->EmitFinish(FinishReason::error);
                break;
            }
        }
    }
    else
    {
        if (ReadLine(fd, line, buffer, ctx->cancelled, timeout_ms_))
        {
            try
            {
                json resp = json::parse(line);
                if (resp.contains("error") && resp["error"].is_object())
                {
                    int code = resp["error"].value("code", 0);
                    if (code != 0)
                    {
                        ctx->error_message = resp["error"].value("message", "stackflow error");
                        ctx->EmitFinish(FinishReason::error);
                    }
                }
                if (resp.contains("data"))
                {
                    const auto &data = resp["data"];
                    if (data.is_string())
                        ctx->final_text = data.get<std::string>();
                    else if (data.is_object())
                        ctx->final_text = data.value("delta", "");
                }
                if (!ctx->finished.load(std::memory_order_acquire))
                    ctx->EmitFinish(FinishReason::stop);
            }
            catch (...)
            {
                ctx->error_message = "StackFlowEngine: response parse failed";
                ctx->EmitFinish(FinishReason::error);
            }
        }
        else
        {
            ctx->EmitFinish(ctx->cancelled ? FinishReason::cancelled : FinishReason::error);
        }
    }

    send_exit();
    ::close(fd);
}
