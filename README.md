# EdgeLLM-Serving

## What

EdgeLLM-Serving 是一个轻量的 LLM Serving / Model API 平台：提供 OpenAI 兼容的 HTTP/SSE 接口，并在同一套 API 下统一封装本地 `llama.cpp` 推理与 StackFlow 远程（RPC）推理。

## Why

- 同时覆盖“单机可跑”和“远程可扩展”：开发/演示走本地，部署演进走 RPC。
- 协议层、会话与调度、推理后端解耦，便于维护与排障。
- 收口主线接口与治理口径，做到可展示、可接手、可复述。

## Features

- OpenAI 兼容：`/v1/chat/completions`（非流式 / SSE）、`/v1/embeddings`、`/v1/rerank`
- 平台治理：`/v1/models`、`/healthz`、admin status、metrics
- 本地 / RPC 双后端：请求级 `inference_backend` 切换
- Session 串行 + model/backend 维度排队，SSE 输出与 `finish_reason` 透传
- 可选 Redis session 持久化（用于重启恢复历史）

## Quick Start

Build:

```bash
cmake -S . -B build
cmake --build build -j
```

Run:

```bash
./build/serving/http/serving_http_server
```

Try:

```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"qwen3.5-2b\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}" | jq
```

Stream:

```bash
curl -sS -N -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"qwen3.5-2b\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}"
```

Remote（StackFlow）与更多启动/配置说明见文档：[`docs/user-guide/快速开始.md`](docs/user-guide/快速开始.md)。

## Docs Index

从这里开始：[`docs/README.md`](docs/README.md)

- User Guide: [`docs/user-guide/README.md`](docs/user-guide/README.md)
- Architecture: [`docs/architecture/README.md`](docs/architecture/README.md)
- Experimental: [`docs/experimental/README.md`](docs/experimental/README.md)
- Notes（面试稿/压测/复盘/roadmap）: [`docs/notes/README.md`](docs/notes/README.md)
