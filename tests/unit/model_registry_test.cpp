#include "engine/ModelRegistry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <algorithm>
#include <utility>

#define EXPECT_TRUE(cond)                                                                       \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            std::cerr << "EXPECT_TRUE failed: " << #cond << " at line " << __LINE__ << "\n"; \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

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
    const auto path = std::filesystem::temp_directory_path() / ("edge_model_registry_test_" + std::to_string(++seq));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    return path;
}

std::filesystem::path write_config(const std::string &content)
{
    const auto temp_dir = make_temp_dir();
    const auto config_path = temp_dir / "config.json";
    std::ofstream out(config_path);
    out << content;
    out.close();
    return config_path;
}

bool has_capability(const ModelInfo &info, const std::string &capability)
{
    return std::find(info.capabilities.begin(), info.capabilities.end(), capability) != info.capabilities.end();
}

const ModelInfo *find_model(const std::vector<ModelInfo> &models, const std::string &id)
{
    for (const auto &model : models)
    {
        if (model.id == id)
            return &model;
    }
    return nullptr;
}

bool test_rpc_backend_must_be_declared()
{
    const auto config_path = write_config(R"json({
  "default_model": "local-only",
  "models": {
    "local-only": {
      "default_backend": "local",
      "capabilities": ["chat"],
      "backends": {
        "local": {
          "engine": "llama",
          "model_path": "models/local.gguf",
          "capabilities": ["chat"]
        }
      }
    }
  }
})json");
    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());

    const ModelSpec rpc_spec = ModelRegistry::Resolve("local-only", "rpc");
    EXPECT_FALSE(rpc_spec.valid);
    EXPECT_FALSE(ModelRegistry::SupportsCapability("local-only", ModelCapability::Chat, "rpc"));

    const ModelSpec local_spec = ModelRegistry::Resolve("local-only", "local");
    EXPECT_TRUE(local_spec.valid);
    EXPECT_EQ(local_spec.backend, std::string("local"));

    std::error_code ec;
    std::filesystem::remove_all(config_path.parent_path(), ec);
    return true;
}

bool test_legacy_rpc_backend_must_be_declared()
{
    const auto config_path = write_config(R"json({
  "default_model": "legacy-local",
  "models": {
    "legacy-local": {
      "backend": "local",
      "engine": "llama",
      "model_path": "models/local.gguf"
    }
  }
})json");
    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());

    const ModelSpec rpc_spec = ModelRegistry::Resolve("legacy-local", "rpc");
    EXPECT_FALSE(rpc_spec.valid);

    std::error_code ec;
    std::filesystem::remove_all(config_path.parent_path(), ec);
    return true;
}

bool test_rpc_does_not_expose_embeddings_or_rerank()
{
    const auto config_path = write_config(R"json({
  "default_model": "multi",
  "models": {
    "multi": {
      "default_backend": "local",
      "capabilities": ["chat", "embeddings", "rerank"],
      "backends": {
        "local": {
          "engine": "llama",
          "model_path": "models/local.gguf",
          "capabilities": ["chat", "embeddings", "rerank"]
        },
        "rpc": {
          "engine": "stackflow",
          "host": "127.0.0.1",
          "port": 10001,
          "capabilities": ["chat", "embeddings", "rerank"]
        }
      }
    }
  }
})json");
    const ScopedEnvVar scoped_config("CONFIG_PATH", config_path.string());

    const ModelSpec rpc_spec = ModelRegistry::Resolve("multi", "rpc");
    EXPECT_TRUE(rpc_spec.valid);
    EXPECT_EQ(rpc_spec.backend, std::string("stackflow"));
    EXPECT_TRUE(ModelRegistry::SupportsCapability("multi", ModelCapability::Chat, "rpc"));
    EXPECT_FALSE(ModelRegistry::SupportsCapability("multi", ModelCapability::Embeddings, "rpc"));
    EXPECT_FALSE(ModelRegistry::SupportsCapability("multi", ModelCapability::Rerank, "rpc"));

    const auto models = ModelRegistry::ListModelInfos();
    const ModelInfo *model = find_model(models, "multi");
    EXPECT_TRUE(model != nullptr);
    EXPECT_TRUE(model->has_local);
    EXPECT_TRUE(model->has_rpc);
    EXPECT_TRUE(has_capability(*model, "chat"));
    EXPECT_TRUE(has_capability(*model, "embeddings"));
    EXPECT_TRUE(has_capability(*model, "rerank"));

    std::error_code ec;
    std::filesystem::remove_all(config_path.parent_path(), ec);
    return true;
}
} // namespace

int main()
{
    bool ok = true;
    ok = ok && test_rpc_backend_must_be_declared();
    ok = ok && test_legacy_rpc_backend_must_be_declared();
    ok = ok && test_rpc_does_not_expose_embeddings_or_rerank();

    if (!ok)
        return 1;

    std::cout << "model_registry_test passed\n";
    return 0;
}
