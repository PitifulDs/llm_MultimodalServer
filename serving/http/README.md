# HTTP Gateway（Serving v2）

本模块是 Edge-LLM-Infra 的 **北向接入适配层（Northbound Adapter）**。

它的唯一作用是：
- 接收外部 HTTP / SSE 请求（curl / Python / Web 等）
- 将 HTTP 请求 **转换为 StackFlows 的 RPC / 事件**
- 将 StackFlows 的流式事件 **转发回 HTTP 客户端**

---

## 一、模块职责（必须遵守）

### 本模块【只允许】做的事情

- HTTP / SSE 协议解析与响应
- 请求参数校验与基础转换
- HTTP 请求 → StackFlows RPC / Event 封装
- StackFlows PUB/SUB 事件 → HTTP 流式输出
- request_id / session_id 的透传与映射

---

## 二、模块边界（禁止事项）

本模块 **严禁** 出现以下行为或依赖：

- ❌ 不包含任何推理逻辑
- ❌ 不包含 unit-manager 相关逻辑
- ❌ 不直接访问 Node / Task
- ❌ 不加载或操作模型（如 llama.cpp）
- ❌ 不管理 session 生命周期
- ❌ 不进行调度、路由或资源分配

所有 **调度 / Session / KV Cache / 推理执行**  
必须由 StackFlows 与 unit-manager 负责。

---

## 三、设计原则

- 本模块必须保持 **无状态**
- 本模块是 **协议适配层，而非服务核心**
- HTTP Gateway 只是 StackFlows 的一个客户端
- 所有外部请求必须通过 StackFlows 进入系统

---

## 四、架构定位说明

在整体架构中，本模块位于：
---
    外部用户（HTTP）
        ↓
    HTTP Gateway（本模块）
        ↓
    StackFlows（ZMQ / RPC / Flow）
        ↓
    unit-manager
        ↓
    Node / Task / Model
---
## 五、重要说明

如果你发现自己想在这里：
- 加模型推理
- 加 unit 选择逻辑
- 加 session 管理

**说明设计方向已经错了，请立刻回退。**
-

# Serving v2（HTTP + Streaming SSE）架构说明
## 1. 背景与目标

本模块是 llm_MultimodalServer 的 Serving v2 实现，目标是提供一个：
- 工程级（非 demo）的 HTTP Server
- 支持 JSON Body 的非流式请求
- 支持 Streaming（SSE）形式的模型推理输出
- 能对接后端 hybrid-comm / ZMQ / RPC LLM Worker
- 能正确处理 TCP 分包、HTTP Body、长连接等真实网络场景
- 本次工作重点解决了 HTTP Server 从“假设一次 read 完整请求”升级为“严格遵循 HTTP/TCP 语义”的关键问题，并完整打通了 streaming SSE 链路。

## 2. 总体架构
```
            ┌────────────┐
            │   Client   │
            │ (curl/web) │
            └─────┬──────┘
                  │ HTTP / SSE
                  ▼
        ┌─────────────────────┐
        │  NetworkHttpServer  │
        │  (HTTP over TCP)    │
        └─────┬───────────────┘
              │ HttpRequest / HttpResponse
              ▼
        ┌─────────────────────┐
        │     HttpGateway     │
        │  (Routing & Logic)  │
        └─────┬───────────────┘
              │ RPC / ZMQ
              ▼
        ┌─────────────────────┐
        │  LLM Worker / Unit  │
        │ (Streaming Output)  │
        └─────────────────────┘

```
Serving v2 的核心职责是 正确处理 HTTP 协议边界，并将请求可靠地转换为后端推理流。
-
## 3. 核心设计要点
### 3.1 TCP 与 HTTP 的边界处理（关键）
HTTP 是基于 TCP 的流协议，TCP 并不保证：
- header 和 body 同时到达
- 一次 read 就是完整请求
  
因此 Serving v2 采用以下设计：
- 每个 TCP 连接维护独立的 HTTP buffer
- 所有 onMessage 回调只做一件事：
👉 累积数据到连接级 buffer
- HTTP 解析只在 buffer 中进行
- 仅当满足以下条件时才进入业务层：
  header 完整（\r\n\r\n）
  body 收齐（基于 Content-Length）
- 成功解析后，消费 buffer 中已处理的数据
  
这彻底解决了以下问题：
- req.body 为空
- JSON parse 偶发失败
- TCP 分包导致的随机错误
----
### 3.2 HTTP Body 解析策略
Serving v2 不假设一次 read 即完整请求，而是：
- 从 header 中解析 Content-Length
- 判断 buffer 中数据是否 ≥ header + body
- body 未收齐时直接返回，等待后续 TCP 数据
- body 收齐后再构造 HttpRequest
- 这是一个 工程级 HTTP Server 的必要条件。
----
### 3.3 Streaming（SSE）支持
对于 stream=true 的请求：
- 返回 Content-Type: text/event-stream
- 使用 Connection: keep-alive
- Gateway 层建立 HttpStreamSession
- 后端通过 ZMQ / RPC 持续推送事件
- 每个事件通过 SSE 写回客户端
  
非流式请求则使用普通 HTTP JSON 响应。
----
### 3.4 Gateway 分层设计
Serving v2 将职责清晰拆分：
- **NetworkHttpServer**
  - TCP / HTTP 协议处理
  - buffer 管理
  - request / response 构造

- **HttpGateway**
  - 路由分发（如 /v1/completions）
  - JSON 解析
  - RPC / ZMQ 调用
  - streaming session 管理

- **HttpRequest / HttpResponse**
  - 屏蔽底层 network 细节
  - 为上层逻辑提供统一接口

这种分层方式便于后续扩展更多 northbound 接口。

## 4. 关键实现点（本次沉淀）
本次工作中完成并验证的关键点包括：
- ✅ 基于 Content-Length 的 HTTP Body 解析
- ✅ 连接级 HTTP buffer + 消费模型
- ✅ 正确的 HTTP/1.1 Header 输出（避免 HTTP/0.9 误判）
- ✅ 非流式 / 流式请求分支
- ✅ Streaming SSE 全链路打通
- ✅ Gateway 关键路径日志（HTTP / JSON / RPC / SSE）

## 5. 当前能力
目前 Serving v2 已支持：
- POST /v1/completions
- JSON body 请求
- stream=true 的 SSE 流式推理 
- 对接 hybrid-comm / ZMQ LLM Worker
- curl / Web 客户端稳定访问

## 5.1 配置项（环境变量）
- `HTTP_PORT`：服务端口（默认 8080，可被命令行 argv[1] 覆盖）
- `WORKER_THREADS`：推理工作线程数（默认 4）
- `DEFAULT_MODEL`：缺省模型名（默认 `llama`）
- `LLAMA_MODEL_PATH`：llama 模型路径（默认内置路径）
- `LLAMA_N_CTX`：上下文长度（默认 4096）
- `LLAMA_N_THREADS`：推理线程数（默认 4）
- `LLAMA_N_THREADS_BATCH`：batch 线程数（默认 4）
- `KV_RESET_MARGIN`：KV cache 逼近 n_ctx 的重建阈值（默认 256）

## 5.1.1 config.json（启动时读取）
默认读取根目录 `config.json`，也可通过环境变量 `CONFIG_PATH` 指定路径。
解析后会写入对应的环境变量（再由现有逻辑使用）。

示例（与当前默认值一致）：
```json
{
  "http_port": 8080,
  "default_model": "llama",
  "worker_threads": 4,
  "max_model_queue": 64,
  "max_session_pending": 64,
  "max_queue_wait_ms": 2000,
  "llama_model_path": "/path/to/model.gguf",
  "llama_n_ctx": 4096,
  "llama_n_threads": 4,
  "llama_n_threads_batch": 4,
  "kv_reset_margin": 256,
  "default_max_tokens": 512
}
```
- `DEFAULT_MAX_TOKENS`：默认生成上限（默认 512，可被请求 `max_tokens` 覆盖）
- `MAX_MODEL_QUEUE`：单模型队列上限（默认 64）
- `MAX_SESSION_PENDING`：单 session 队列上限（默认 64）
- `MAX_QUEUE_WAIT_MS`：队列等待超时（默认 2000ms）

## 6. 健康检查与指标
- `GET /health`：返回服务状态与启动时长
- `GET /metrics`：返回简单聚合指标（请求数/并发/平均耗时等）

错误返回统一结构（示例）：
```json
{
  "error": {
    "message": "messages must be array",
    "type": "invalid_request_error",
    "code": "invalid_messages"
  }
}
```
并配合对应 HTTP 状态码（400/404/405/429/500/501）。

## 7. Web Demo 使用（Windows 访问 VM）
Demo 页面与 API 是两个服务，**端口不能相同**：
- Demo 静态页：`8000`
- API 服务：`8080`（或 config.json / 启动参数指定）

### 7.1 启动 API
```bash
./serving/build/http/serving_http_server 8080
```

### 7.2 启动 Demo 页面
```bash
bash demo/web/serve_demo.sh 8000
```

### 7.3 Windows 浏览器访问
假设 VM IP 为 `192.168.110.128`：
- 页面地址：`http://192.168.110.128:8000/`
- API 地址：`http://192.168.110.128:8080`

**注意**：
- 页面里 API 地址不能用 `127.0.0.1` 或 `localhost`，那会指向 Windows 本机。
- 8080 若无法访问，请检查 VM 防火墙或虚拟网络设置。
- `MAX_QUEUE_WAIT_MS`：队列等待超时（默认 2000ms）
```
示例：
curl -N -X POST "http://127.0.0.1:8080/v1/completions?stream=true" \
  -H "Content-Type: application/json" \
  -d '{"model":"dummy","prompt":"Tell me a joke"}'

## 5.2 压测脚本（SSE）
```
python3 sample/stress_sse.py --concurrency 30 --rounds 500 --abort-ratio 0.7 --abort-min 0.2 --abort-max 2.5
```

## 5.3 Web Demo
静态页面位于 `demo/web/index.html`，可直接用浏览器打开，或使用本地静态服务器：
```
python3 -m http.server 8000 -d demo/web
```
默认请求 `http://127.0.0.1:8080/v1/chat/completions?stream=true`。
```

## 6. Client ↔ Server 请求 / Streaming 时序图
```
Client                         Server (Serving v2)
  |                                   |
  |  HTTP POST /v1/completions        |
  |  (JSON body, Content-Length)     |
  |---------------------------------->|
  |                                   |
  |        TCP read (partial?)         |
  |                                   |
  |                                   |
  |        append to http_buffer       |
  |                                   |
  |        ┌─────────────────────┐    |
  |        │  header complete ?  │    |
  |        └─────────┬───────────┘    |
  |                  │ no             |
  |                  ▼                |
  |           wait more TCP data      |
  |                                   |
  |        TCP read (more bytes)       |
  |                                   |
  |        append to http_buffer       |
  |                                   |
  |        ┌─────────────────────┐    |
  |        │  header complete ?  │    |
  |        └─────────┬───────────┘    |
  |                  │ yes            |
  |                  ▼                |
  |        parse Content-Length        |
  |                                   |
  |        ┌────────────────────────┐ |
  |        │ body received fully ?  │ |
  |        └─────────┬──────────────┘ |
  |                  │ no              |
  |                  ▼                 |
  |            wait more TCP data      |
  |                                    |
  |        TCP read (remaining body)   |
  |                                    |
  |        append to http_buffer       |
  |                                    |
  |        ┌────────────────────────┐ |
  |        │ body received fully ?  │ |
  |        └─────────┬──────────────┘ |
  |                  │ yes             |
  |                  ▼                 |
  |        extract header + body       |
  |        consume buffer              |
  |                                    |
  |        construct HttpRequest       |
  |        construct HttpResponse      |
  |                                    |
  |        route: /v1/completions      |
  |                                    |
  |        ┌──────────────────────┐   |
  |        │ stream == true ?     │   |
  |        └─────────┬────────────┘   |
  |                  │ yes             |
  |                  ▼                 |
  |        send SSE response header    |
  |<----------------------------------|
  |  HTTP/1.1 200 OK                  |
  |  Content-Type: text/event-stream  |
  |  Connection: keep-alive            |
  |                                    |
  |        create HttpStreamSession    |
  |        send RPC(stream=true)       |
  |---------------------------------->| (ZMQ / RPC)
  |                                    |
  |        receive worker event        |
  |        (delta / done)              |
  |                                    |
  |        format SSE: data: {...}     |
  |<----------------------------------|
  |  data: {"delta":"..."}             |
  |                                    |
  |        receive next event          |
  |<----------------------------------|
  |  data: {"delta":"..."}             |
  |                                    |
  |        receive done event          |
  |<----------------------------------|
  |  data: {"done":true}               |
  |                                    |
  |        close session / unsubscribe |
  |                                    |

```
