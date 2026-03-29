#include "engine/StackFlowEngine.h"

#include "utils/json.hpp"
#include "serving/core/Session.h"
#include <glog/logging.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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

    int estimate_tokens_from_text(const std::string &text)
    {
        if (text.empty())
            return 0;
        return std::max(1, static_cast<int>((text.size() + 3) / 4));
    }

    FinishReason parse_finish_reason(const json &data, FinishReason fallback = FinishReason::stop)
    {
        if (!data.is_object())
            return fallback;
        const std::string reason = data.value("finish_reason", "");
        if (reason == "length")
            return FinishReason::length;
        if (reason == "cancelled")
            return FinishReason::cancelled;
        if (reason == "error")
            return FinishReason::error;
        if (reason == "stop")
            return FinishReason::stop;
        return fallback;
    }
} // namespace

StackFlowEngine::StackFlowEngine()
{
    Options options;
    options.host = get_env_string("STACKFLOW_HOST", "127.0.0.1");
    options.port = get_env_int("STACKFLOW_PORT", 10001);
    options.unit_name = get_env_string("STACKFLOW_UNIT", "llm");
    options.response_format = get_env_string("STACKFLOW_RESPONSE_FORMAT", "llm.utf-8");
    options.response_format_stream = get_env_string("STACKFLOW_RESPONSE_FORMAT_STREAM", "llm.utf-8.stream");
    options.timeout_ms = get_env_int("STACKFLOW_TIMEOUT_MS", 10000);
    options.infer_timeout_ms = get_env_int("STACKFLOW_INFER_TIMEOUT_MS", 0);
    options.reuse_work_id = get_env_int("STACKFLOW_REUSE_WORK_ID", 1) != 0;
    options.serialize_reuse = get_env_int("STACKFLOW_SERIALIZE_REUSE", 1) != 0;

    host_ = options.host;
    port_ = options.port;
    unit_name_ = options.unit_name;
    response_format_ = options.response_format;
    response_format_stream_ = options.response_format_stream;
    timeout_ms_ = options.timeout_ms;
    infer_timeout_ms_ = options.infer_timeout_ms;
    reuse_work_id_ = options.reuse_work_id;
    serialize_reuse_ = options.serialize_reuse;
}

StackFlowEngine::StackFlowEngine(const Options &options)
{
    host_ = options.host;
    port_ = options.port;
    unit_name_ = options.unit_name;
    response_format_ = options.response_format;
    response_format_stream_ = options.response_format_stream;
    timeout_ms_ = options.timeout_ms;
    infer_timeout_ms_ = options.infer_timeout_ms;
    reuse_work_id_ = options.reuse_work_id;
    serialize_reuse_ = options.serialize_reuse;
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

static json BuildMessagesPayload(const std::vector<Message> &messages)
{
    json j;
    j["messages"] = json::array();
    for (const auto &m : messages)
    {
        j["messages"].push_back({{"role", m.role}, {"content", m.content}});
    }
    return j;
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

    std::unique_lock<std::mutex> reuse_lk(reuse_mu_, std::defer_lock);
    if (reuse_work_id_ && serialize_reuse_)
    {
        reuse_lk.lock();
    }

    std::vector<Message> full_messages;
    if (ctx->is_chat)
    {
        if (ctx->session)
        {
            std::lock_guard<std::mutex> lk(ctx->session->mu);
            full_messages = ctx->session->history;
        }
        full_messages.insert(full_messages.end(), ctx->messages.begin(), ctx->messages.end());
        if (full_messages.empty())
            full_messages = ctx->messages;
    }

    std::string payload;
    json payload_json;
    std::string system_prompt;
    if (ctx->is_chat && !full_messages.empty())
    {
        payload_json = BuildMessagesPayload(full_messages);
        payload = payload_json.dump();
        system_prompt.clear();
    }
    else
    {
        payload = ctx->prompt;
        system_prompt.clear();
    }

    ctx->usage.prompt_tokens = estimate_tokens_from_text(payload);
    ctx->usage.completion_tokens = 0;
    ctx->usage.total_tokens = ctx->usage.prompt_tokens;

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
    int infer_timeout_ms = infer_timeout_ms_ > 0 ? infer_timeout_ms_ : timeout_ms_;
    if (!ctx->stream && infer_timeout_ms_ <= 0)
    {
        const long long by_tokens = static_cast<long long>(max_tokens) * 200;
        if (by_tokens > infer_timeout_ms)
            infer_timeout_ms = static_cast<int>(std::min<long long>(by_tokens, 120000));
    }

    auto set_param_number = [&](json &j, const char *key)
    {
        auto it = ctx->params.find(key);
        if (it == ctx->params.end())
            return;
        try
        {
            double v = std::stod(it->second);
            j[key] = v;
        }
        catch (...)
        {
        }
    };

    auto set_param_int = [&](json &j, const char *key)
    {
        auto it = ctx->params.find(key);
        if (it == ctx->params.end())
            return;
        try
        {
            long long v = std::stoll(it->second);
            j[key] = v;
        }
        catch (...)
        {
        }
    };

    json setup_data = {
        {"model", ctx->model.empty() ? unit_name_ : ctx->model},
        {"response_format", response_format},
        {"input", response_format},
        {"enoutput", true},
        {"max_token_len", max_tokens},
        {"prompt", system_prompt}
    };

    set_param_number(setup_data, "temperature");
    set_param_number(setup_data, "top_p");
    set_param_int(setup_data, "top_k");
    if (ctx->params.find("repeat_penalty") != ctx->params.end())
        set_param_number(setup_data, "repeat_penalty");
    else
        set_param_number(setup_data, "repetition_penalty");
    set_param_number(setup_data, "presence_penalty");
    set_param_number(setup_data, "frequency_penalty");
    set_param_int(setup_data, "repeat_last_n");
    set_param_int(setup_data, "seed");

    const std::string setup_key = response_format + "|" + setup_data.dump();

    json setup_req = {
        {"request_id", ctx->request_id},
        {"work_id", unit_name_},
        {"action", "setup"},
        {"object", "llm.setup"},
        {"data", setup_data}
    };

    if (reuse_work_id_)
    {
        std::lock_guard<std::mutex> lk(work_mu_);
        const bool is_stream = ctx->stream;
        const std::string &cached_key = is_stream ? cached_setup_key_stream_ : cached_setup_key_nostream_;
        if (!cached_key.empty() && cached_key == setup_key)
        {
            work_id = is_stream ? cached_work_id_stream_ : cached_work_id_nostream_;
        }
    }

    if (work_id.empty())
    {
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
                    if (code == -21 || code == -26)
                    {
                        ctx->params["error_code"] = "overloaded";
                    }
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

        if (reuse_work_id_)
        {
            std::lock_guard<std::mutex> lk(work_mu_);
            if (ctx->stream)
            {
                cached_work_id_stream_ = work_id;
                cached_setup_key_stream_ = setup_key;
            }
            else
            {
                cached_work_id_nostream_ = work_id;
                cached_setup_key_nostream_ = setup_key;
            }
        }
    }

    json infer_req;
    infer_req["request_id"] = ctx->request_id;
    infer_req["work_id"] = work_id;
    infer_req["action"] = "inference";
    infer_req["object"] = response_format;
    if (ctx->stream)
    {
        if (ctx->is_chat && !payload_json.is_null())
        {
            infer_req["data"] = {{"delta", payload_json}, {"index", 0}, {"finish", true}};
        }
        else
        {
            infer_req["data"] = {{"delta", payload}, {"index", 0}, {"finish", true}};
        }
    }
    else
    {
        infer_req["data"] = payload;
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
                    if (!ctx->cancelled.load(std::memory_order_acquire) && ctx->error_message.empty())
                    {
                        ctx->error_message = "StackFlowEngine: stream inference timeout or connection closed";
                    }
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
                        if (code == -21 || code == -26)
                        {
                            ctx->params["error_code"] = "overloaded";
                        }
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
                    const FinishReason finish_reason = parse_finish_reason(data);
                    if (!delta.empty())
                    {
                        ctx->EmitDelta(delta);
                        ctx->usage.completion_tokens = estimate_tokens_from_text(ctx->final_text);
                        ctx->usage.total_tokens = ctx->usage.prompt_tokens + ctx->usage.completion_tokens;
                    }
                    if (finish)
                    {
                        ctx->usage.completion_tokens = estimate_tokens_from_text(ctx->final_text);
                        ctx->usage.total_tokens = ctx->usage.prompt_tokens + ctx->usage.completion_tokens;
                        ctx->EmitFinish(finish_reason);
                        break;
                    }
                }
                else if (data.is_string())
                {
                    ctx->EmitDelta(data.get<std::string>());
                    ctx->usage.completion_tokens = estimate_tokens_from_text(ctx->final_text);
                    ctx->usage.total_tokens = ctx->usage.prompt_tokens + ctx->usage.completion_tokens;
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
        if (ReadLine(fd, line, buffer, ctx->cancelled, infer_timeout_ms))
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
                        if (code == -21 || code == -26)
                        {
                            ctx->params["error_code"] = "overloaded";
                        }
                        ctx->EmitFinish(FinishReason::error);
                    }
                }
                if (resp.contains("data"))
                {
                    const auto &data = resp["data"];
                    if (data.is_string())
                    {
                        ctx->final_text = data.get<std::string>();
                    }
                    else if (data.is_object())
                    {
                        ctx->final_text = data.value("delta", "");
                    }
                }
                ctx->usage.completion_tokens = estimate_tokens_from_text(ctx->final_text);
                ctx->usage.total_tokens = ctx->usage.prompt_tokens + ctx->usage.completion_tokens;
                if (!ctx->finished.load(std::memory_order_acquire))
                {
                    FinishReason finish_reason = FinishReason::stop;
                    if (resp.contains("data"))
                    {
                        finish_reason = parse_finish_reason(resp["data"]);
                    }
                    ctx->EmitFinish(finish_reason);
                }
            }
            catch (...)
            {
                ctx->error_message = "StackFlowEngine: response parse failed";
                ctx->EmitFinish(FinishReason::error);
            }
        }
        else
        {
            if (!ctx->cancelled.load(std::memory_order_acquire) && ctx->error_message.empty())
            {
                ctx->error_message = "StackFlowEngine: inference timeout or connection closed";
            }
            ctx->EmitFinish(ctx->cancelled ? FinishReason::cancelled : FinishReason::error);
        }
    }

    if (!reuse_work_id_)
    {
        send_exit();
    }
    ::close(fd);
}

std::mutex StackFlowEngine::reuse_mu_;
