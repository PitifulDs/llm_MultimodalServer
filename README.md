# EdgeLLM-Serving

一个轻量的模型 API 平台，覆盖 **本地 llama.cpp 推理** + **StackFlow 远程推理**，提供 OpenAI 风格 HTTP/SSE 接口。当前主线聚焦 `chat`、`embeddings`、`rerank` 三类能力，以及 `/v1/models`、`/healthz`、admin status 等平台治理接口。

**项目展示点**
- OpenAI 兼容 `/v1/chat/completions`（流式 / 非流式）
- OpenAI 兼容 `/v1/embeddings`
- OpenAI 兼容 `/v1/rerank`
- `GET /v1/models`、`GET /healthz`、admin status
- HTTP 正确解析与 SSE 推流
- 本地 / 远程双后端统一抽象
- 配置化启动与可运维脚本

**技术栈**
- C++17, nlohmann/json, glog
- llama.cpp（本地推理）
- 自研 TCP 网络层（reactor 风格）
- StackFlow RPC over TCP（远程推理）

**第三方组件说明**
- llama.cpp 作为本地模型运行时
- network、unit-manager 属于项目基础模块，我实现了 serving/gateway/engine 的整合，以及 demo worker 的功能补齐

---

**架构（分层视图）**

```mermaid
flowchart TB
  C[Client / Browser / curl]

  subgraph S[Serving 进程]
    N[NetworkHttpServer\nTCP/HTTP 解析]
    G[HttpGateway\n路由/会话/SSE]
    P[ChatRequestParser\n请求转 ServingContext]
    SX[SessionExecutor\n同 session 串行]
    AX[AgentExecutor\n扩展 agent]
    E[EngineExecutor\n按 model+backend 排队]
    L[LlamaEngine\n本地 llama.cpp]
    SF[StackFlowEngine\n远程 RPC 客户端]

    N --> G --> P
    G --> SX
    SX --> AX
    SX --> E
    AX --> E
    E --> L
    E --> SF
  end

  subgraph R[Remote 推理链路]
    U[unit-manager\n路由与生命周期]
    W[node/test worker\n模型执行]
    H[hybrid-comm]
    U --> W
    W --> H
    H --> W
  end

  C -->|HTTP/SSE| N
  SF -->|TCP/RPC| U
```

**目录结构（定位视图）**

```text
EdgeLLM-Serving/
├─ serving/
│  ├─ http/              # OpenAI 协议接入、SSE 输出
│  └─ core/              # session、调度、ServingContext（含 agent/rag 兼容逻辑）
├─ engine/               # LlamaEngine / StackFlowEngine / Factory
├─ network/              # Reactor 网络库（EventLoop/Poller/Channel）
├─ unit-manager/         # 远程调度与 worker 管理
├─ node/test/            # demo worker（远程推理执行）
├─ hybrid-comm/          # 远程通信封装
├─ scripts/              # start_all/stop_all 等脚本
└─ docs/                 # 架构、设计模式、面试问答
```

当前说明文档:
- `docs/模型API平台化改造文档.md`
- `docs/系统架构.md`
- `docs/API调用示例.md`
- `docs/本地推理与RPC推理.md`
- `serving/http/使用说明.md`

**一次请求链路**
1. `NetworkHttpServer` 严格按 `Content-Length` 组包（不猜测 body 长度，不支持 `Transfer-Encoding: chunked`）。
2. `HttpGateway + ChatRequestParser` 构造 `ServingContext`。
3. `SessionExecutor` 保证同一 `session_id` 串行。
4. chat 主链路直接进入 `EngineExecutor`；embeddings / rerank 走各自 service；agent/rag 仅作为兼容扩展保留。
5. `EngineExecutor` 按 `capability + model + inference_backend` 维度排队并复用 engine。
6. `LlamaEngine` 或 `StackFlowEngine` 执行推理。
7. `OpenAIStreamWriter` 输出 SSE，或 `HttpGateway` 输出普通 JSON。

---

**快速运行（本地 llama.cpp）**

Build:
```bash
cmake -S . -B build
cmake --build build -j
```

Run（请在仓库根目录运行，确保相对路径生效）:
```bash
./build/serving/http/serving_http_server
```

Test:
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"qwen3.5-2b\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}" | jq
```

Health:
```bash
curl -s "http://127.0.0.1:8080/healthz" | jq
```

Admin status:
```bash
curl -s "http://127.0.0.1:8080/admin/models/status" | jq
curl -s "http://127.0.0.1:8080/admin/backends/status" | jq
```

流式（OpenAI 兼容写法，body 带 `"stream": true`）:
```bash
curl -sS -N -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"qwen3.5-2b\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}"
```

Embeddings:
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/embeddings" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"qwen3.5-2b\",\"input\":\"hello embeddings\"}" | jq
```

Rerank:
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/rerank" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"qwen3.5-2b\",\"query\":\"hello rerank\",\"documents\":[\"totally unrelated weather report\",\"hello rerank\"],\"top_n\":1}" | jq
```

列出当前模型目录:
```bash
curl -s "http://127.0.0.1:8080/v1/models" | jq
```

主线 smoke：
```bash
bash scripts/smoke_test.sh
```

治理口径（chat / embeddings / rerank 共用）：
- 日志统一字段：`request_id`、`api`、`model`、`backend`、`capability`、`session_id`、`queue_wait_ms`、`run_ms`、`finish_reason`、`status_code`、`error_code`
- 主线错误码：`model_required`、`invalid_input`、`invalid_query`、`invalid_documents`、`invalid_top_n`、`model_not_found`、`capability_not_supported`、`backend_not_available`、`request_timeout`、`backend_timeout`、`request_cancelled`、`queue_full`、`queue_timeout`、`rate_limit_global`、`rate_limit_model`、`rate_limit_session`、`internal_error`
- `/metrics`、`/admin/models/status`、`/admin/backends/status` 统一累计三条主链路的错误、超时、取消、限流与 token 统计

---

**远程模式（StackFlow）**

启动顺序:
1) `unit-manager`
```bash
./unit-manager/build/unit_manager
```

2) worker（`node/test`）
```bash
./node/test/build/test
```

3) HTTP server
```bash
./build/serving/http/serving_http_server
```

或者使用脚本:
```bash
bash scripts/start_all.sh
```

主线自检：
```bash
bash scripts/smoke_test.sh
```

`start_all.sh` 会读取 `config.json`，解析相对路径为绝对路径，导出 `STACKFLOW_MODEL_PATH` / `LLAMA_MODEL_PATH` / `LLM_MODEL_PATH` / `STACKFLOW_MAX_CONCURRENCY`，并自动设置 `LD_LIBRARY_PATH`；随后清理旧 socket，将日志写到 `/tmp/llm_serving`。

---

**配置说明**

`config.json` 启动时加载，路径默认相对仓库根目录。

推荐把请求里的 `model` 当成“逻辑模型名”，例如 `qwen3.5-2b`。  
服务端会根据 `config.json` 中的 `models` 注册表解析后端能力，再根据请求中的 `inference_backend` 决定走本地 `llama.cpp` 还是远程 `stackflow`。

示例：
- `model = qwen3.5-2b, inference_backend = local` -> 本地 `llama.cpp`
- `model = qwen3.5-2b, inference_backend = rpc` -> 远程 `stackflow`

常用配置:
- `http_port`, `default_model`
- `models`（模型注册表，按模型名解析 backend / engine / model_path）
- `llama_model_path`, `llama_n_ctx`, `llama_n_threads`, `llama_n_threads_batch`
- `default_max_tokens`, `kv_reset_margin`
- `rag_index_path`, `rag_vector_index_path`, `rag_chunk_metadata_path`
- `rag_embeddings_path`, `rag_id_map_path`
- `rag_default_top_k`, `rag_default_mode`, `rag_default_fusion`, `rag_max_context_chars`
- `rag_enable_neighbor_expand`, `rag_max_neighbor_count`, `rag_enable_retrieval_debug_api`
- `experimental_agent_api_enabled`, `experimental_rag_api_enabled`
- `serving_backend`（`local` 或 `stackflow`）
- `stackflow_host`, `stackflow_port`, `stackflow_unit`
- `stackflow_timeout_ms`, `stackflow_infer_timeout_ms`
- `stackflow_reuse_work_id`, `stackflow_serialize_reuse`, `stackflow_max_concurrency`
- `session_persist_redis`, `redis_host`, `redis_port`, `redis_db`
- `session_redis_prefix`, `session_redis_ttl_seconds`, `redis_timeout_ms`

模型注册表示例：
```json
{
  "default_model": "qwen3.5-2b",
  "models": {
    "qwen3.5-2b": {
      "backend": "local",
      "engine": "llama",
      "model_path": "models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf"
    }
  }
}
```

这样同一个 HTTP 服务里可以按“模型 + 后端开关”切换：
- 请求 `"model":"qwen3.5-2b","inference_backend":"local"` 时走本地
- 请求 `"model":"qwen3.5-2b","inference_backend":"rpc"` 时走远程
- 如果请求里不带 `inference_backend`，则按模型注册表中的默认映射解析

说明：
- `/v1/models` 会返回当前模型注册表中的模型名
- `backends` 表示该逻辑模型在配置里的已声明后端能力（模型级）
- `gateway_backends` 表示网关支持的请求级后端切换模式（路由级）
- 前端应通过 `inference_backend` 在同一个逻辑模型上切换后端
- `stackflow` 远程模式下的 `usage` 目前是基于文本长度的近似统计，不是精确 tokenizer 结果

`inference_backend=rpc` 解析规则（当前）：
- 若模型在配置中显式声明了 rpc/stackflow 后端，则按该声明解析
- 若模型未显式声明 rpc，当前仍会走全局 `STACKFLOW_*` / `config.json` 的 stackflow fallback
- 因此 `backends` 是“模型声明能力”，`gateway_backends` 是“网关路由能力”

`-remote` 兼容策略（当前决定）：
- `config.json` 主配置不再推荐使用 `*-remote` 模型名
- `ModelRegistry` 仍暂时保留 `*-remote` 的兼容解析分支，保证历史请求不立即中断
- 后续会在一次单独迁移中移除这层兼容逻辑

**扩展能力（兼容保留，默认不占主线）**

agent/rag 仍保留兼容能力，但已经退出主 README 的默认操作路径：
- `POST /v1/chat/completions` 仍兼容 `agent=true` 与 `rag` 扩展字段
- `GET /health` 保留为兼容别名，主入口改为 `GET /healthz`
- `POST /v1/agent/debug`、`POST /v1/retrieval/search`、`GET /admin/rag/status`、`POST /admin/rag/reload-index`
  默认关闭；需要显式设置 `EXPERIMENTAL_AGENT_API_ENABLED=1` 或 `EXPERIMENTAL_RAG_API_ENABLED=1`

扩展验证脚本与说明：
- `bash scripts/smoke_test_agent_code_analysis.sh`
- `bash scripts/smoke_test_agent_web_research.sh`
- `bash scripts/smoke_test_rag.sh`
- `bash scripts/smoke_test_rag_v2.sh`
- `docs/智能体使用说明.md`
- `docs/调试说明.md`

**流式请求兼容说明**

- OpenAI 兼容方式：在 JSON body 里携带 `"stream": true`
- 兼容兜底：也接受 query `?stream=true`
- 若 body 里显式给出 `stream`，以 body 为准（覆盖 query）

**Redis 会话持久化（可选）**

用于把 `session history` 落到 Redis，支持进程重启后恢复历史上下文。

开启方式（`config.json`）:
```json
{
  "session_persist_redis": 1,
  "redis_host": "127.0.0.1",
  "redis_port": 6379,
  "redis_db": 0,
  "session_redis_prefix": "edge:session:",
  "session_redis_ttl_seconds": 1800,
  "redis_timeout_ms": 1000
}
```

行为说明:
- `SessionManager::getOrCreate` 会优先从 Redis 拉取历史并恢复。
- 请求成功结束（`stop/length`）后会写回最新历史到 Redis。
- `SessionManager::close` 会删除对应 Redis key。

---

**代码讲解（面试版）**

**1) HTTP 解析与请求生命周期**
- 文件: `serving/http/NetworkHttpServer.cc`
- 作用: 连接级 buffer + Content-Length 组包，避免 TCP 分包导致的 JSON 解析失败。

**2) 路由与请求校验**
- 文件: `serving/http/HttpGateway.cc`
- 作用: OpenAI 格式解析、session diff、参数校验、流式回写。

**3) SSE 输出格式**
- 文件: `serving/http/OpenAIStreamWriter.cc`
- 作用: 将内部 chunk 转成 OpenAI SSE 规范，并在结束时输出 `[DONE]`。

**4) 线程与调度**
- 文件: `serving/core/EngineExecutor.cc`
- 作用: 推理任务放入 worker 线程，避免 IO 线程阻塞。

**5) 本地推理封装**
- 文件: `engine/LlamaEngine.cc`
- 作用: llama.cpp 包装，支持 max_tokens、temperature、top_p、top_k 等采样参数。

**6) 远程推理协议**
- 文件: `engine/StackFlowEngine.cc`
- 作用: TCP 交互（setup → inference → exit），支持 work_id 复用和串行保护。

**7) Worker 行为**
- 文件: `node/test/src/llm_task.cc`
- 作用: 加载模型、推理输出、UTF-8 清洗、并发控制（`try_begin_infer`）。


---

**底层模块说明（network / unit-manager / infra-controller / hybrid-comm）**

**network**
- 位置: `network/`
- 职责: TCP 事件循环、Channel/Acceptor/Buffer、连接管理，整体是 reactor 风格。
- 在本项目中: `NetworkHttpServer` 和 `StackFlowEngine` 都依赖它的非阻塞 IO。

**unit-manager**
- 位置: `unit-manager/`
- 职责: 管理 worker 生命周期、接收 RPC、维护 work_id 与通道信息。
- 在本项目中: `StackFlowEngine` 通过它完成 setup/inference/exit。

**hybrid-comm**
- 位置: `hybrid-comm/`
- 职责: ZMQ/RPC 通信封装与消息序列化，作为远程推理的数据通道。
- 在本项目中: worker 推理事件通过它回传。

**infra-controller**
- 位置: `infra-controller/`
- 职责: 进程/节点级的控制与编排（偏基础设施层）。
- 在本项目中: 目前主要作为基础模块存在，Serving 层只依赖其接口或构建产物。

---

**详细文档**

- `docs/系统架构.md`：架构、时序图、模块调用链
- `docs/设计模式.md`：项目中使用到的设计模式与代码示例
- `docs/面试问答.md`：面试高频问题与回答模板
- `docs/项目亮点.md`：开场亮点与可量化结果
- `docs/技术取舍.md`：关键技术取舍与方案对比
- `docs/问题复盘.md`：真实问题复盘与修复策略
- `docs/性能压测报告.md`：压测方法、结果与吞吐估算
- `docs/API调用示例.md`：可直接复制的 API 调用示例
- `docs/可观测性与排障.md`：日志、指标与排障流程
- `docs/面试讲解稿.md`：1/3/8 分钟面试讲解稿
- `docs/本地推理与RPC推理.md`：两种推理后端的差异、优势与适用场景

**排查与日志**
- 清理 socket:
```bash
rm -f /tmp/llm/*.sock*
```
- `start_all.sh` 日志目录: `/tmp/llm_serving`

---

**后续可提升点**
- Token batching 与调度优化
- 更完整的监控指标（QPS / 延迟 / 错误率）
- Prompt 模板与多轮角色规范化

---

**面试可讲的重点**
- 我完成了 HTTP/SSE 的工程化实现，并正确处理 TCP 分包。
- 通过统一 ServingContext 抽象接入本地与远程推理。
- 我把 IO 与推理解耦，避免阻塞事件循环。
- 我把配置、脚本与日志打包成可演示的完整项目。

---

**单元测试**

当前提供主线治理与接口测试；其中 embeddings / rerank 治理测试会校验 timeout、cancelled、metrics、admin status 等平台口径。

```bash
cmake -S . -B build
cmake --build build --target \
  http_utils_test \
  http_gateway_governance_test \
  embeddings_gateway_test \
  rerank_gateway_test \
  admin_status_gateway_test \
  rag_test -j
./build/tests/unit/http_utils_test
./build/tests/unit/http_gateway_governance_test
./build/tests/unit/embeddings_gateway_test
./build/tests/unit/rerank_gateway_test
./build/tests/unit/admin_status_gateway_test
./build/tests/unit/rag_test
python3 tests/unit/rag_chunkers_test.py
```

**Smoke 测试（服务自检）**

```bash
BASE_URL=http://127.0.0.1:8080 MODEL=llama bash scripts/smoke_test.sh
```

可选参数：`BASE_URL` / `MODEL` / `TIMEOUT`。

扩展 smoke：
```bash
BASE_URL=http://127.0.0.1:8080 MODEL=llama bash scripts/smoke_test_rag.sh
BASE_URL=http://127.0.0.1:8080 MODEL=llama bash scripts/smoke_test_rag_v2.sh
```

---

**压测数据（2026-02-23）**

`llama`（SSE，`sample/stress_sse.py`）
- 场景 A：`concurrency=6, rounds=30, abort_ratio=0`
- 结果：`total=30, ok=30, failed=0, aborted=0, avg_dur_s=15.437, avg_bytes=2691.4`
- 场景 B：`concurrency=10, rounds=80, abort_ratio=0.4`
- 结果：`total=80, ok=80, failed=0, aborted=33, avg_dur_s=15.907, avg_bytes=1606.7`

原始日志：
- `/tmp/llm_serving/bench_llama_stream_full.txt`
- `/tmp/llm_serving/bench_llama_stream_mixabort.txt`

`stackflow`（SSE，stable）
- 场景：`concurrency=2, rounds=20, abort_ratio=0`
- 结果：`total=20, ok=20, failed=0, aborted=0, avg_dur_s=2.199, avg_bytes=7439.6`
- 日志：`/tmp/llm_serving/bench_stackflow_stream_stable.txt`
