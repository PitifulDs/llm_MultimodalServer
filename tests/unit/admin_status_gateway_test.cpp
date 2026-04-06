#include "serving/http/HttpGateway.h"
#include "serving/http/http_types.h"

#include "engine/EngineFactory.h"
#include "utils/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
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
    std::function<void()> on_close;

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
        return true;
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
        on_close = std::move(cb);
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
    const auto path = std::filesystem::temp_directory_path() / ("edge_admin_status_gateway_test_" + std::to_string(++seq));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    return path;
}

std::string repo_model_path()
{
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::current_path() / "models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf",
        std::filesystem::current_path() / "../models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf",
        std::filesystem::current_path() / "../../models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf",
    };
    for (const auto &candidate : candidates)
    {
        std::error_code ec;
        const auto normalized = std::filesystem::weakly_canonical(candidate, ec);
        const auto &path = ec ? candidate : normalized;
        if (std::filesystem::exists(path))
            return path.string();
    }
    return "models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf";
}

bool test_admin_status_uses_unified_backend_runtime_fields()
{
    const auto temp_dir = make_temp_dir();
    const auto config_path = temp_dir / "config.json";

    std::ofstream out(config_path);
    out << "{\n"
           "  \"default_model\": \"platform-model\",\n"
           "  \"models\": {\n"
           "    \"platform-model\": {\n"
           "      \"default_backend\": \"local\",\n"
           "      \"capabilities\": [\"chat\", \"embeddings\", \"rerank\"],\n"
           "      \"backends\": {\n"
           "        \"local\": {\n"
           "          \"engine\": \"llama\",\n"
           "          \"model_path\": \"" << repo_model_path() << "\",\n"
           "          \"capabilities\": [\"chat\", \"embeddings\", \"rerank\"]\n"
           "        }\n"
           "      }\n"
           "    }\n"
           "  }\n"
           "}\n";
    out.close();

    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());
    EngineFactory::ClearCache();

    HttpGateway gateway;

    FakeRequest embeddings_req;
    embeddings_req.body = R"json({
        "model":"platform-model",
        "backend":"local",
        "input":"admin status embeddings"
    })json";
    FakeResponse embeddings_res;
    gateway.HandleEmbeddings(embeddings_req, embeddings_res);
    EXPECT_EQ(embeddings_res.status, 200);

    FakeRequest rerank_req;
    rerank_req.body = R"json({
        "model":"platform-model",
        "backend":"local",
        "query":"admin status rerank",
        "documents":["other document","admin status rerank"]
    })json";
    FakeResponse rerank_res;
    gateway.HandleRerank(rerank_req, rerank_res);
    EXPECT_EQ(rerank_res.status, 200);

    FakeRequest model_status_req;
    FakeResponse model_status_res;
    gateway.HandleAdminModelsStatus(model_status_req, model_status_res);
    EXPECT_EQ(model_status_res.status, 200);
    const auto model_status = json::parse(model_status_res.body);
    EXPECT_EQ(model_status["data"].size(), static_cast<size_t>(1));
    EXPECT_EQ(model_status["data"][0]["id"].get<std::string>(), std::string("platform-model"));
    EXPECT_TRUE(model_status["data"][0]["registered"].get<bool>());
    EXPECT_TRUE(model_status["data"][0]["available"].get<bool>());
    EXPECT_EQ(model_status["data"][0]["default_backend"].get<std::string>(), std::string("local"));
    EXPECT_EQ(model_status["data"][0]["gateway_default_backend"].get<std::string>(), std::string("local"));
    EXPECT_TRUE(model_status["data"][0]["capabilities"].is_array());
    EXPECT_TRUE(model_status["data"][0]["declared_backends"].is_array());
    EXPECT_EQ(model_status["data"][0]["available_backends"][0].get<std::string>(), std::string("local"));
    EXPECT_EQ(model_status["data"][0]["failure_summary"].get<std::string>(), std::string(""));

    FakeRequest backend_status_req;
    FakeResponse backend_status_res;
    gateway.HandleAdminBackendsStatus(backend_status_req, backend_status_res);
    EXPECT_EQ(backend_status_res.status, 200);
    const auto backend_status = json::parse(backend_status_res.body);
    EXPECT_EQ(backend_status["data"].size(), static_cast<size_t>(1));
    EXPECT_EQ(backend_status["data"][0]["backend"].get<std::string>(), std::string("local"));
    EXPECT_EQ(backend_status["data"][0]["gateway_backend"].get<std::string>(), std::string("local"));
    EXPECT_TRUE(backend_status["data"][0]["connected"].get<bool>());
    EXPECT_EQ(backend_status["data"][0]["model_count"].get<int>(), 1);
    EXPECT_TRUE(backend_status["data"][0]["capabilities"].is_array());
    EXPECT_TRUE(backend_status["data"][0]["loaded_engine_count"].is_number_integer());
    EXPECT_TRUE(backend_status["data"][0]["queue_length"].get<int>() >= 0);
    EXPECT_EQ(backend_status["data"][0]["requests_total"].get<int>(), 2);
    EXPECT_EQ(backend_status["data"][0]["requests_error_total"].get<int>(), 0);
    EXPECT_EQ(backend_status["data"][0]["requests_cancelled_total"].get<int>(), 0);
    EXPECT_EQ(backend_status["data"][0]["requests_timeout_total"].get<int>(), 0);
    EXPECT_EQ(backend_status["data"][0]["requests_rate_limited_total"].get<int>(), 0);
    EXPECT_EQ(backend_status["data"][0]["last_error"].get<std::string>(), std::string(""));
    EXPECT_EQ(backend_status["data"][0]["timeout_total"].get<int>(), 0);
    EXPECT_EQ(backend_status["data"][0]["cancelled_total"].get<int>(), 0);
    EXPECT_TRUE(backend_status["data"][0]["prompt_tokens_total"].get<int>() > 0);
    EXPECT_EQ(backend_status["data"][0]["completion_tokens_total"].get<int>(), 0);
    EXPECT_TRUE(backend_status["data"][0]["total_tokens_total"].get<int>() > 0);
    EXPECT_EQ(backend_status["data"][0]["requests_in_flight"].get<int>(), 0);

    FakeRequest metrics_req;
    FakeResponse metrics_res;
    gateway.HandleMetrics(metrics_req, metrics_res);
    EXPECT_EQ(metrics_res.status, 200);
    const auto metrics = json::parse(metrics_res.body);
    EXPECT_EQ(metrics["requests_total"].get<int>(), 2);
    EXPECT_EQ(metrics["requests_in_flight"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_stream_total"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_error_total"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_cancelled_total"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_timeout_total"].get<int>(), 0);
    EXPECT_EQ(metrics["requests_rate_limited_total"].get<int>(), 0);
    EXPECT_TRUE(metrics["prompt_tokens_total"].get<int>() > 0);
    EXPECT_EQ(metrics["completion_tokens_total"].get<int>(), 0);
    EXPECT_TRUE(metrics["total_tokens_total"].get<int>() > 0);
    EXPECT_TRUE(metrics["avg_latency_ms"].is_number());

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    return true;
}
} // namespace

int main()
{
    bool ok = true;
    ok = ok && test_admin_status_uses_unified_backend_runtime_fields();

    if (!ok)
        return 1;

    std::cout << "admin_status_gateway_test passed\n";
    return 0;
}
