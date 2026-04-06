#include "serving/http/HttpGateway.h"
#include "serving/http/http_types.h"

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
    const auto path = std::filesystem::temp_directory_path() / ("edge_embeddings_gateway_test_" + std::to_string(++seq));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    return path;
}

bool test_chat_only_model_returns_400()
{
    const auto temp_dir = make_temp_dir();
    const auto config_path = temp_dir / "config.json";

    std::ofstream out(config_path);
    out << R"json({
  "default_model": "chat-only",
  "models": {
    "chat-only": {
      "backend": "local",
      "engine": "llama",
      "model_path": "models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf"
    }
  }
})json";
    out.close();

    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());

    HttpGateway gateway;
    FakeRequest req;
    req.body = R"json({
        "model":"chat-only",
        "input":"hello"
    })json";

    FakeResponse res;
    gateway.HandleEmbeddings(req, res);
    EXPECT_EQ(res.status, 400);

    const auto response = json::parse(res.body);
    EXPECT_EQ(response["error"]["code"].get<std::string>(), std::string("capability_not_supported"));

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    return true;
}

bool test_local_llama_embeddings_returns_200()
{
    const auto temp_dir = make_temp_dir();
    const auto config_path = temp_dir / "config.json";

    std::ofstream out(config_path);
    out << R"json({
  "default_model": "qwen3.5-2b",
  "models": {
    "qwen3.5-2b": {
      "default_backend": "local",
      "capabilities": ["chat", "embeddings"],
      "backends": {
        "local": {
          "engine": "llama",
          "model_path": "models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf",
          "capabilities": ["chat", "embeddings"]
        }
      }
    }
  }
})json";
    out.close();

    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());

    HttpGateway gateway;
    FakeRequest req;
    req.body = R"json({
        "model":"qwen3.5-2b",
        "backend":"local",
        "input":"hello embeddings"
    })json";

    FakeResponse res;
    gateway.HandleEmbeddings(req, res);
    EXPECT_EQ(res.status, 200);

    const auto response = json::parse(res.body);
    EXPECT_EQ(response["object"].get<std::string>(), std::string("list"));
    EXPECT_EQ(response["model"].get<std::string>(), std::string("qwen3.5-2b"));
    EXPECT_TRUE(response["data"].is_array());
    EXPECT_EQ(response["data"].size(), static_cast<size_t>(1));
    EXPECT_TRUE(response["data"][0]["embedding"].is_array());
    EXPECT_TRUE(!response["data"][0]["embedding"].empty());
    EXPECT_TRUE(response["usage"]["prompt_tokens"].get<int>() > 0);
    EXPECT_EQ(response["usage"]["total_tokens"].get<int>(), response["usage"]["prompt_tokens"].get<int>());

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    return true;
}
} // namespace

int main()
{
    bool ok = true;
    ok = ok && test_chat_only_model_returns_400();
    ok = ok && test_local_llama_embeddings_returns_200();

    if (!ok)
        return 1;

    std::cout << "embeddings_gateway_test passed\n";
    return 0;
}
