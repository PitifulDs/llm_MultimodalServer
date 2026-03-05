#include "network/EventLoop.h"
#include "network/InetAddress.h"

#include "NetworkHttpServer.h"
#include "HttpGateway.h"
#include "StackFlowsClient.h"

// #include "engine/DummyEngine.h"
#include "engine/RpcEngine.h"
#include "engine/EngineFactory.h"

#include "../../utils/json.hpp"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <iostream>
using json = nlohmann::json;

/*
    1.只负责启动
    2.不写 handler 逻辑
    3.不包含业务代码
*/

namespace
{
    void set_env_from_json(const json &cfg, const char *key, const char *env)
    {
        if (!cfg.contains(key))
            return;

        std::string v;
        if (cfg[key].is_string())
            v = cfg[key].get<std::string>();
        else if (cfg[key].is_number_integer())
            v = std::to_string(cfg[key].get<int>());
        else
            return;

        setenv(env, v.c_str(), 1); // config 作为主配置
    }

    void load_config()
    {
        const char *cfg_path = std::getenv("CONFIG_PATH");
        if (!cfg_path || !*cfg_path)
            cfg_path = "config.json";

        std::ifstream in(cfg_path);
        if (!in.is_open())
        {
            std::cerr << "[serving-http] config not found: " << cfg_path << std::endl;
            return;
        }

        try
        {
            json cfg = json::parse(in);
            set_env_from_json(cfg, "http_port", "HTTP_PORT");
            set_env_from_json(cfg, "default_model", "DEFAULT_MODEL");
            set_env_from_json(cfg, "worker_threads", "WORKER_THREADS");
            set_env_from_json(cfg, "max_model_queue", "MAX_MODEL_QUEUE");
            set_env_from_json(cfg, "max_session_pending", "MAX_SESSION_PENDING");
            set_env_from_json(cfg, "max_queue_wait_ms", "MAX_QUEUE_WAIT_MS");
            set_env_from_json(cfg, "llama_model_path", "LLAMA_MODEL_PATH");
            set_env_from_json(cfg, "llama_n_ctx", "LLAMA_N_CTX");
            set_env_from_json(cfg, "llama_n_threads", "LLAMA_N_THREADS");
            set_env_from_json(cfg, "llama_n_threads_batch", "LLAMA_N_THREADS_BATCH");
            set_env_from_json(cfg, "kv_reset_margin", "KV_RESET_MARGIN");
            set_env_from_json(cfg, "default_max_tokens", "DEFAULT_MAX_TOKENS");
            set_env_from_json(cfg, "serving_backend", "SERVING_BACKEND");
            set_env_from_json(cfg, "stackflow_host", "STACKFLOW_HOST");
            set_env_from_json(cfg, "stackflow_port", "STACKFLOW_PORT");
            set_env_from_json(cfg, "stackflow_unit", "STACKFLOW_UNIT");
            set_env_from_json(cfg, "stackflow_timeout_ms", "STACKFLOW_TIMEOUT_MS");
            set_env_from_json(cfg, "stackflow_infer_timeout_ms", "STACKFLOW_INFER_TIMEOUT_MS");
            set_env_from_json(cfg, "stackflow_reuse_work_id", "STACKFLOW_REUSE_WORK_ID");
            set_env_from_json(cfg, "stackflow_serialize_reuse", "STACKFLOW_SERIALIZE_REUSE");
            set_env_from_json(cfg, "stackflow_max_concurrency", "STACKFLOW_MAX_CONCURRENCY");
            set_env_from_json(cfg, "warmup_model", "WARMUP_MODEL");
            set_env_from_json(cfg, "session_persist_redis", "SESSION_PERSIST_REDIS");
            set_env_from_json(cfg, "redis_host", "REDIS_HOST");
            set_env_from_json(cfg, "redis_port", "REDIS_PORT");
            set_env_from_json(cfg, "redis_db", "REDIS_DB");
            set_env_from_json(cfg, "session_redis_prefix", "SESSION_REDIS_PREFIX");
            set_env_from_json(cfg, "session_redis_ttl_seconds", "SESSION_REDIS_TTL_SECONDS");
            set_env_from_json(cfg, "redis_timeout_ms", "REDIS_TIMEOUT_MS");
            std::cerr << "[serving-http] config loaded: " << cfg_path << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[serving-http] config parse failed: " << e.what() << std::endl;
        }
    }
} // namespace

int main(int argc, char **argv)
{
    // 先加载 config.json（再允许 argv 覆盖）
    load_config();

    // HTTP Server 初始化（后续实现）
    // StackFlowsClient 初始化
    // 路由注册
    uint16_t port = 8080;
    if (const char *env_port = std::getenv("HTTP_PORT"))
    {
        port = static_cast<uint16_t>(std::stoi(env_port));
    }
    if (argc > 1)
    {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    network::EventLoop loop;

    // 模型 warmup（可关闭）
    bool warmup = true;
    if (const char *v = std::getenv("WARMUP_MODEL"))
    {
        warmup = std::string(v) != "0";
    }
    if (warmup)
    {
        std::cout << "[serving-http] warming up model..." << std::endl;
        EngineFactory::Create("llama");
        std::cout << "[serving-http] warmup done" << std::endl;
    }
    else
    {
        std::cout << "[serving-http] warmup skipped" << std::endl;
    }

    HttpGateway gateway;

    network::InetAddress listen_addr(port);
    NetworkHttpServer server(&loop, listen_addr, &gateway);

    std::cout << "[serving-http] listen on port " << port << std::endl;

    server.Start();
    loop.loop();
    return 0;
}
