#include "serving/http/HttpGateway.h"
#include "serving/http/http_types.h"

#include "engine/EngineFactory.h"
#include "utils/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

using json = nlohmann::json;

#define EXPECT_TRUE(cond)                                                                       \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            std::cerr << "EXPECT_TRUE failed: " << #cond << " at line " << __LINE__ << "\n"; \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

#define EXPECT_EQ(a, b)                                                                                     \
    do                                                                                                       \
    {                                                                                                        \
        if (!((a) == (b)))                                                                                   \
        {                                                                                                    \
            std::cerr << "EXPECT_EQ failed: " << #a << " vs " << #b << " at line " << __LINE__ << "\n"; \
            return false;                                                                                    \
        }                                                                                                    \
    } while (0)

namespace
{
struct FakeRequest : HttpRequest
{
    std::unordered_map<std::string, std::string> query;

    bool HasQuery(const std::string &key) const override
    {
        return query.find(key) != query.end();
    }

    std::string Query(const std::string &key) const override
    {
        const auto it = query.find(key);
        return it == query.end() ? std::string() : it->second;
    }
};

struct FakeResponse : HttpResponse
{
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    bool ended = false;

    mutable std::mutex mu;
    std::condition_variable cv;
    std::function<void()> on_close;
    bool on_close_registered = false;
    std::atomic<bool> alive{true};

    void SetHeader(const std::string &key, const std::string &value) override
    {
        headers[key] = value;
    }

    void Write(const std::string &data) override
    {
        body += data;
    }

    bool IsAlive() const override
    {
        return alive.load(std::memory_order_acquire);
    }

    void SetStatus(int code, const std::string &reason = "") override
    {
        (void)reason;
        status = code;
    }

    void End() override
    {
        ended = true;
    }

    void SetOnClose(std::function<void()> cb) override
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            on_close = std::move(cb);
            on_close_registered = true;
        }
        cv.notify_all();
    }

    bool WaitForOnClose(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [this]
                           { return on_close_registered; });
    }

    void CloseConnection()
    {
        alive.store(false, std::memory_order_release);
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lk(mu);
            cb = on_close;
        }
        if (cb)
            cb();
    }
};

struct ScopedEnvVar
{
    std::string name;
    bool had_old = false;
    std::string old_value;

    ScopedEnvVar(std::string env_name, const std::string &value)
        : name(std::move(env_name))
    {
        if (const char *existing = std::getenv(name.c_str()))
        {
            had_old = true;
            old_value = existing;
        }
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar()
    {
        if (had_old)
            setenv(name.c_str(), old_value.c_str(), 1);
        else
            unsetenv(name.c_str());
    }
};

std::filesystem::path make_temp_dir()
{
    static int seq = 0;
    const auto path = std::filesystem::temp_directory_path() / ("edge_http_governance_test_" + std::to_string(++seq));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    return path;
}

std::filesystem::path write_dummy_config()
{
    const auto temp_dir = make_temp_dir();
    const auto config_path = temp_dir / "config.json";
    std::ofstream out(config_path);
    out << R"json({
  "default_model": "dummy-model",
  "models": {
    "dummy-model": {
      "backend": "dummy",
      "engine": "dummy"
    }
  }
})json";
    out.close();
    return config_path;
}

FakeRequest make_chat_request(const std::string &session_id)
{
    FakeRequest req;
    req.body = "{"
               "\"model\":\"dummy-model\","
               "\"session_id\":\"" + session_id + "\","
               "\"messages\":[{\"role\":\"user\",\"content\":\"hello governance\"}]"
               "}";
    return req;
}

json read_metrics(HttpGateway &gateway)
{
    FakeRequest metrics_req;
    FakeResponse metrics_res;
    gateway.HandleMetrics(metrics_req, metrics_res);
    if (metrics_res.status != 200)
    {
        std::cerr << "failed to read metrics, status=" << metrics_res.status
                  << " body=" << metrics_res.body << "\n";
        return json::object();
    }
    return json::parse(metrics_res.body);
}

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

bool test_chat_request_timeout_records_metrics()
{
    const auto config_path = write_dummy_config();
    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());
    const ScopedEnvVar scoped_timeout("HTTP_REQUEST_TIMEOUT_MS", "150");
    EngineFactory::ClearCache();

    HttpGateway gateway;
    FakeRequest req = make_chat_request("timeout-session");
    FakeResponse res;
    gateway.HandleChatCompletion(req, res);

    EXPECT_EQ(res.status, 504);
    const auto response = json::parse(res.body);
    EXPECT_EQ(response["error"]["code"].get<std::string>(), std::string("request_timeout"));

    const auto metrics = read_metrics(gateway);
    EXPECT_EQ(metrics["requests_total"].get<int>(), 1);
    EXPECT_EQ(metrics["requests_error_total"].get<int>(), 1);
    EXPECT_EQ(metrics["requests_timeout_total"].get<int>(), 1);
    EXPECT_EQ(metrics["requests_rate_limited_total"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_cancelled_total"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_in_flight"].get<int>(), 0);

    std::error_code ec;
    std::filesystem::remove_all(config_path.parent_path(), ec);
    return true;
}

bool test_chat_disconnect_cancels_and_records_metrics()
{
    const auto config_path = write_dummy_config();
    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());
    EngineFactory::ClearCache();

    HttpGateway gateway;
    FakeRequest req = make_chat_request("disconnect-session");
    FakeResponse res;
    std::thread worker([&gateway, &req, &res]()
                       { gateway.HandleChatCompletion(req, res); });

    EXPECT_TRUE(res.WaitForOnClose(std::chrono::seconds(1)));
    res.CloseConnection();
    worker.join();

    EXPECT_TRUE(wait_until([&gateway]()
                           {
                               const auto metrics = read_metrics(gateway);
                               return metrics["requests_cancelled_total"].get<int>() == 1 &&
                                      metrics["requests_in_flight"].get<int>() == 0;
                           },
                           std::chrono::seconds(2)));

    const auto metrics = read_metrics(gateway);
    EXPECT_EQ(metrics["requests_total"].get<int>(), 1);
    EXPECT_EQ(metrics["requests_cancelled_total"].get<int>(), 1);
    EXPECT_EQ(metrics["requests_error_total"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_timeout_total"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_rate_limited_total"].get<int>(), 0);

    std::error_code ec;
    std::filesystem::remove_all(config_path.parent_path(), ec);
    return true;
}

bool test_chat_global_concurrency_limit()
{
    const auto config_path = write_dummy_config();
    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());
    const ScopedEnvVar scoped_limit("MAX_CONCURRENT_REQUESTS", "1");
    EngineFactory::ClearCache();

    HttpGateway gateway;
    FakeRequest first_req = make_chat_request("global-limit-a");
    FakeResponse first_res;
    std::thread worker([&gateway, &first_req, &first_res]()
                       { gateway.HandleChatCompletion(first_req, first_res); });

    EXPECT_TRUE(first_res.WaitForOnClose(std::chrono::seconds(1)));

    FakeRequest second_req = make_chat_request("global-limit-b");
    FakeResponse second_res;
    gateway.HandleChatCompletion(second_req, second_res);
    EXPECT_EQ(second_res.status, 429);
    const auto response = json::parse(second_res.body);
    EXPECT_EQ(response["error"]["code"].get<std::string>(), std::string("rate_limit_global"));

    first_res.CloseConnection();
    worker.join();
    EXPECT_TRUE(wait_until([&gateway]()
                           { return read_metrics(gateway)["requests_in_flight"].get<int>() == 0; },
                           std::chrono::seconds(2)));

    const auto metrics = read_metrics(gateway);
    EXPECT_EQ(metrics["requests_total"].get<int>(), 2);
    EXPECT_EQ(metrics["requests_rate_limited_total"].get<int>(), 1);

    std::error_code ec;
    std::filesystem::remove_all(config_path.parent_path(), ec);
    return true;
}

bool test_chat_model_concurrency_limit()
{
    const auto config_path = write_dummy_config();
    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());
    const ScopedEnvVar scoped_limit("MAX_MODEL_CONCURRENCY", "1");
    EngineFactory::ClearCache();

    HttpGateway gateway;
    FakeRequest first_req = make_chat_request("model-limit-a");
    FakeResponse first_res;
    std::thread worker([&gateway, &first_req, &first_res]()
                       { gateway.HandleChatCompletion(first_req, first_res); });

    EXPECT_TRUE(first_res.WaitForOnClose(std::chrono::seconds(1)));

    FakeRequest second_req = make_chat_request("model-limit-b");
    FakeResponse second_res;
    gateway.HandleChatCompletion(second_req, second_res);
    EXPECT_EQ(second_res.status, 429);
    const auto response = json::parse(second_res.body);
    EXPECT_EQ(response["error"]["code"].get<std::string>(), std::string("rate_limit_model"));

    first_res.CloseConnection();
    worker.join();
    EXPECT_TRUE(wait_until([&gateway]()
                           { return read_metrics(gateway)["requests_in_flight"].get<int>() == 0; },
                           std::chrono::seconds(2)));

    const auto metrics = read_metrics(gateway);
    EXPECT_EQ(metrics["requests_total"].get<int>(), 2);
    EXPECT_EQ(metrics["requests_rate_limited_total"].get<int>(), 1);

    std::error_code ec;
    std::filesystem::remove_all(config_path.parent_path(), ec);
    return true;
}

bool test_chat_session_concurrency_limit()
{
    const auto config_path = write_dummy_config();
    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());
    const ScopedEnvVar scoped_limit("MAX_SESSION_CONCURRENCY", "1");
    EngineFactory::ClearCache();

    HttpGateway gateway;
    FakeRequest first_req = make_chat_request("shared-session");
    FakeResponse first_res;
    std::thread worker([&gateway, &first_req, &first_res]()
                       { gateway.HandleChatCompletion(first_req, first_res); });

    EXPECT_TRUE(first_res.WaitForOnClose(std::chrono::seconds(1)));

    FakeRequest second_req = make_chat_request("shared-session");
    FakeResponse second_res;
    gateway.HandleChatCompletion(second_req, second_res);
    EXPECT_EQ(second_res.status, 429);
    const auto response = json::parse(second_res.body);
    EXPECT_EQ(response["error"]["code"].get<std::string>(), std::string("rate_limit_session"));

    first_res.CloseConnection();
    worker.join();
    EXPECT_TRUE(wait_until([&gateway]()
                           { return read_metrics(gateway)["requests_in_flight"].get<int>() == 0; },
                           std::chrono::seconds(2)));

    const auto metrics = read_metrics(gateway);
    EXPECT_EQ(metrics["requests_total"].get<int>(), 2);
    EXPECT_EQ(metrics["requests_rate_limited_total"].get<int>(), 1);

    std::error_code ec;
    std::filesystem::remove_all(config_path.parent_path(), ec);
    return true;
}
} // namespace

int main()
{
    bool ok = true;
    ok = ok && test_chat_request_timeout_records_metrics();
    ok = ok && test_chat_disconnect_cancels_and_records_metrics();
    ok = ok && test_chat_global_concurrency_limit();
    ok = ok && test_chat_model_concurrency_limit();
    ok = ok && test_chat_session_concurrency_limit();

    if (!ok)
        return 1;

    std::cout << "http_gateway_governance_test passed\n";
    return 0;
}
