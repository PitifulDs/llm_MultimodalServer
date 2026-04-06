# 模型 API 平台化改造文档

## 1. 文档目的

本文档用于把 EdgeLLM-Serving 从“带 agent 能力的 LLM Serving”收敛为“通用本地模型 API 平台 / 模型网关”。

本次改造遵循两个原则：

- 先统一目标、边界、分层和实施顺序，再逐步改代码。
- 不做一次性大重构，优先保留当前可工作的 chat 主链路，在兼容基础上演进。

本文档是后续实施的唯一高层依据。后续代码改造应严格按本文档阶段推进。

## 2. 新定位

### 2.1 项目定位

改造后的项目定位为：

- 本地模型 API 服务
- 多后端模型网关
- OpenAI 风格接口兼容层
- 面向模型能力编排的服务平台

不再把项目定位为：

- 通用 agent 平台
- 以 agent/debug/rag 为主线的对外产品
- 以工作流编排为核心的智能体系统

### 2.2 平台目标

主线目标聚焦三类模型能力：

- `chat`
- `embeddings`
- `rerank`

平台核心价值变为：

- 统一接入本地与远程 backend
- 统一模型注册、路由、状态和治理
- 提供稳定、可观测、可扩展的模型 API

## 3. 当前能力基线盘点

以下盘点以当前仓库实现为准。

### 3.1 应保留并增强的主线能力

- `POST /v1/chat/completions`
- `GET /v1/models`
- HTTP 请求解析与响应封装
- SSE 流式输出
- `SessionExecutor` 的同 session 串行机制
- `EngineExecutor` 按 `model + backend` 维度排队
- `EngineFactory + ModelRegistry` 的模型解析与 engine 复用
- 本地 `llama.cpp` backend
- 远程 `StackFlow` backend
- 配置驱动的模型与 backend 选择

### 3.2 保留但降级为非主线能力

- `agent=true` 透传式 chat 扩展
- `POST /v1/agent/debug`
- `RAGExecutor`
- `/admin/rag/status`
- `/admin/rag/reload-index`
- agent 相关 planner / evidence / formatter / tools
- demo web 中以 agent 为中心的交互逻辑

这些能力短期内不直接删除，但要退出主链路，不再作为 README、接口文档和默认产品心智的重点。

### 3.3 当前主要问题

当前代码可工作，但主线边界不够清晰，主要问题如下：

- `HttpGateway` 同时承载 chat、agent、rag、metrics、admin/debug 等多种职责，API 边界过宽。
- `ServingContext` 已混入大量 agent/rag 字段，主链路上下文和扩展链路上下文未分离。
- `ModelRegistry` 目前只显式表达 local/rpc 和 chat 语义，无法声明 embeddings/rerank 能力。
- `ModelEngine` 只有 `Run(ctx)` 一个抽象，无法自然承载多种模型能力。
- 对外接口范围过散，主线文档和实现重心不够统一。
- 日志、错误码、超时、取消、限流、状态接口等治理能力已有局部实现，但尚未形成统一平台规范。

## 4. 最终对外接口范围

改造完成后的对外主接口只聚焦以下七个：

- `POST /v1/chat/completions`
- `GET /v1/models`
- `POST /v1/embeddings`
- `POST /v1/rerank`
- `GET /healthz`
- `GET /admin/models/status`
- `GET /admin/backends/status`

### 4.1 接口范围说明

- `chat/completions` 继续作为主入口，兼容当前流式和非流式能力。
- `models` 负责模型发现与能力声明。
- `embeddings` 和 `rerank` 是本次平台化新增的正式能力。
- `healthz` 是统一健康检查入口，替代当前 `/health`。
- 两个 `admin` 状态接口只提供平台状态，不暴露 agent/rag 专用语义。

### 4.2 非主线接口处理策略

以下接口不再属于主线对外范围：

- `GET /health`
- `GET /metrics`
- `POST /v1/agent/debug`
- `POST /admin/rag/reload-index`
- `GET /admin/rag/status`
- 其他面向 agent/rag 的调试接口

处理原则如下：

- 第一阶段保留兼容，不立即删除。
- 第二阶段迁移到内部/实验能力区，或通过配置开关控制。
- 第三阶段从主文档、主脚本、主 demo 中移除。

### 4.3 当前实现约束与过渡原则

在进入代码改造前，需要明确当前仓库的真实约束：

- `HttpGateway` 目前仍直接承载 chat、agent、rag、health、metrics、admin 等多类逻辑。
- `ServingContext` 已经深度绑定 chat 运行态，并混入大量 agent/rag 字段。
- `ModelRegistry` 当前只具备“模型名 + backend 解析”能力，还不是平台级模型目录。
- `EngineExecutor + ModelEngine::Run(ctx)` 当前是一条面向 chat 的执行通路。
- `config.json` 仍以旧版模型声明结构为主，尚未进入“逻辑模型 + capability + backend 实现”的平台化形态。

因此本次改造采用以下过渡原则：

- 先建立平台主线骨架，再逐步迁移旧链路；不先推翻现有 chat 主链路。
- 第一阶段优先做逻辑解耦，不强求立即完成大规模目录搬迁。
- 第一阶段允许 `ServingContext` 继续服务 chat 主链路，但 embeddings/rerank 不再继续往其中塞字段。
- 第一阶段允许 `HttpGateway` 仍作为总入口存在，但新增平台主线逻辑必须进入独立 handler / service。
- 新配置结构必须与旧配置结构兼容解析，避免一次性打断现有运行方式。

## 5. 总体改造流程

整体流程固定为五步：

1. 先文档
2. 先立最小平台骨架
3. 再拆 chat 主链路
4. 再扩展正式模型接口
5. 最后治理与降级非主线能力

### 5.1 阶段解释

#### 阶段一：文档

目标：

- 定义新定位
- 确认主线接口
- 明确分层和抽象
- 锁定实施顺序

输出：

- 本文档

#### 阶段二：平台骨架

目标：

- 引入 capability 概念，但不强制一次性替换全部 engine 抽象
- 建立最小可落地的 service / catalog / status 骨架
- 明确新旧配置兼容解析方式
- 不改变现有 `/v1/chat/completions` 外部行为

输出：

- capability-aware 的平台内部对象
- 扩展后的 model catalog 结构
- `healthz` / admin status 可依赖的状态来源

#### 阶段三：chat 主链路拆分

目标：

- 把 `HttpGateway` 中的 chat 主链路下沉到 `ChatService`
- 收敛 chat 专属解析、编排、错误归一化
- 保持 SSE 与非流式兼容

输出：

- chat 主链路和扩展链路的边界
- 仍可运行的主线 chat service

#### 阶段四：正式模型接口扩展

目标：

- 新增 `embeddings`
- 新增 `rerank`
- 完成 capability 校验、路由和最小 backend 接入

输出：

- chat / embeddings / rerank 三条正式模型 API

#### 阶段五：治理与主线收口

目标：

- 统一日志
- 统一错误码
- 完善超时、取消、限流和状态观测
- 让 agent/rag 退出主线

输出：

- 平台化运行质量达标

## 6. 模块分层设计

改造后主线采用三层结构：

- API 层
- Service 层
- Backend 层

## 7. API 层设计

### 7.1 职责

API 层只负责：

- 路由分发
- HTTP 请求解析
- 参数校验
- OpenAI 风格响应序列化
- SSE 输出
- 错误到 HTTP 状态码的映射
- request id 注入

API 层不负责：

- 模型选择策略
- session 编排
- backend 能力判断
- 推理执行细节
- agent/rag 编排

### 7.2 建议结构

建议把当前 `HttpGateway` 中的逻辑拆为如下入口：

- `ChatApiHandler`
- `ModelsApiHandler`
- `EmbeddingsApiHandler`
- `RerankApiHandler`
- `HealthApiHandler`
- `AdminStatusApiHandler`

`HttpGateway` 可以保留为总入口或路由分发器，但不再继续堆积业务逻辑。

## 8. Service 层设计

### 8.1 职责

Service 层负责平台主业务编排：

- 请求上下文构造
- 模型解析与 capability 校验
- session 串行策略
- 调用 backend 执行
- usage、finish_reason、状态对象汇总
- 平台级错误归一化

### 8.2 建议服务划分

- `ChatService`
- `ModelCatalogService`
- `EmbeddingsService`
- `RerankService`
- `HealthService`
- `AdminStatusService`

可选的共享内部组件：

- `RequestContextFactory`
- `ExecutionOrchestrator`
- `BackendRouter`
- `UsageAggregator`
- `PlatformErrorMapper`

### 8.3 与现有模块关系

现有模块的迁移方向如下：

- `ChatRequestParser` 保留，但主职责收敛为 chat 请求解析。
- `SessionExecutor` 保留，作为 chat 主链路基础设施。
- `EngineExecutor` 演进为 capability-aware 的执行编排器。
- `ModelRegistry` 演进为平台模型目录与 capability 注册中心。
- `AgentExecutor` 不再作为主线 service 的依赖。

## 9. Backend 层设计

### 9.1 职责

Backend 层只负责和具体推理后端交互。

它关心的是：

- backend 初始化
- 请求到 backend 协议的转换
- 流式/非流式结果回收
- backend 级超时和取消
- backend 健康状态探测

它不关心：

- HTTP
- OpenAI 错误结构
- 主文档定位
- agent/rag 决策

### 9.2 backend 实体

当前 backend 实体保留两类：

- `llama.cpp`
- `StackFlow`

未来若新增其他 backend，也应走同一抽象层，不应直接在 API 层新增分支。

## 10. backend 能力抽象设计

### 10.1 目标

当前 `ModelEngine` 只有 chat 风格的 `Run(ctx)`，不足以支撑 embeddings 和 rerank。

改造后应把 backend 抽象从“单一生成引擎”升级为“多能力模型 backend”，但不要求第一阶段一次性替换所有旧实现。

### 10.2 能力集合

平台正式支持的 capability 固定为：

- `chat`
- `embeddings`
- `rerank`

### 10.3 建议抽象

最终目标抽象可以表达以下语义：

- `SupportsChat()`
- `SupportsEmbeddings()`
- `SupportsRerank()`
- `RunChat(...)`
- `RunEmbeddings(...)`
- `RunRerank(...)`
- `GetBackendStatus()`

但落地顺序必须分两步：

- 第一步，在现有 `ModelEngine` 之上增加 capability adapter / backend facade，让新 service 不直接依赖 `ServingContext`。
- 第二步，再逐步把具体 backend 实现补齐到统一多能力接口。

第一阶段明确不做的事：

- 不要求立刻删除 `ModelEngine::Run(ctx)`。
- 不要求立刻重写 `LlamaEngine` 和 `StackFlowEngine` 的 chat 实现。
- 不要求 embeddings/rerank 一开始就共用和 chat 完全一致的执行器。

### 10.4 request/response 统一模型

建议在 service 和 backend 之间定义三组明确的内部对象：

- `ChatRequest / ChatResponse`
- `EmbeddingsRequest / EmbeddingsResponse`
- `RerankRequest / RerankResponse`

其中：

- `ServingContext` 在兼容期内只继续承担 chat 主链路运行态。
- embeddings/rerank 从第一版开始就使用独立 request/response 对象。
- agent/rag 不进入新的主线 request/response 抽象。

### 10.5 streaming 策略

- `chat` 支持非流式和 SSE 流式。
- `embeddings` 首期只支持非流式。
- `rerank` 首期只支持非流式。

这能显著降低新增接口的复杂度。

## 11. 模型注册表扩展设计

### 11.1 当前问题

当前 `ModelRegistry` 主要表达：

- 模型名
- local/rpc
- engine 类型
- model_path 或 stackflow 配置

但它还不能直接表达：

- 模型支持哪些 capability
- 同一逻辑模型在不同 backend 下支持的 capability 是否一致
- embeddings 维度、rerank top_n 上限等能力元数据

### 11.2 新注册表目标

模型注册表要同时回答三个问题：

- 这个逻辑模型是什么
- 它支持哪些能力
- 每个能力在各 backend 上如何路由

### 11.3 建议结构

建议把模型声明扩展为“逻辑模型 + backend 实现 + capability 声明”的形式。

逻辑上应支持以下字段：

- `id`
- `default_backend`
- `capabilities`
- `metadata`
- `backends.local`
- `backends.stackflow`

其中 `capabilities` 至少支持：

- `chat`
- `embeddings`
- `rerank`

其中 `metadata` 可逐步扩展：

- `embedding_dimension`
- `max_input_tokens`
- `max_output_tokens`
- `supports_stream`
- `tokenizer_family`

### 11.4 capability 声明原则

- capability 必须以模型为中心声明，不能只靠接口硬编码推断。
- capability 必须允许 backend 级覆盖。
- 同一逻辑模型在 local 与 stackflow 上可能能力不同，平台必须允许这种不对称。

### 11.5 新旧配置兼容策略

当前仓库仍在使用旧配置结构，例如：

```json
{
  "models": {
    "qwen3.5-2b": {
      "backend": "local",
      "engine": "llama",
      "model_path": "models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf"
    }
  }
}
```

平台化后的目标结构建议为：

```json
{
  "default_model": "qwen3.5-2b",
  "models": {
    "qwen3.5-2b": {
      "default_backend": "local",
      "capabilities": ["chat"],
      "metadata": {
        "supports_stream": true
      },
      "backends": {
        "local": {
          "engine": "llama",
          "model_path": "models/qwen3.5/Qwen3.5-2B-Q4_K_M.gguf",
          "capabilities": ["chat"]
        },
        "stackflow": {
          "engine": "stackflow",
          "unit": "llm",
          "capabilities": ["chat"]
        }
      }
    }
  }
}
```

兼容原则如下：

- 第一阶段必须同时支持旧结构和新结构。
- 当配置为旧结构时，`ModelRegistry` 需要自动推导最小 capability 集合。
- 对于旧结构推导出来的能力，首期只保证 `chat`，不隐式推导 `embeddings` 和 `rerank`。
- 新结构优先级高于旧结构；一旦同一模型显式声明 `backends` 和 `capabilities`，实现应以新结构为准。

### 11.6 `/v1/models` 返回设计

`GET /v1/models` 最终应表达：

- 模型基本信息
- 默认 backend
- 支持的 capability
- 已声明 backend 列表
- backend 当前可用性摘要

建议分两阶段返回：

- 第一阶段：返回静态声明信息，如 `id`、`default_backend`、`capabilities`、`backends`。
- 第二阶段：再补动态状态摘要，如 backend availability、最近错误、是否已就绪。

不建议继续只返回 `backends/local/rpc` 的浅层信息。

## 12. `chat/completions` 保留与加固方案

### 12.1 保留原因

`chat/completions` 是当前最成熟、最可用的链路，必须保留，并作为平台主入口继续增强。

### 12.2 加固方向

主线加固点如下：

- 保持 OpenAI 兼容请求格式和响应格式
- 保持当前流式与非流式都可用
- 保持 session 串行与 route queue 机制
- 明确模型不存在、backend 不可用、capability 不支持等错误语义
- 统一 `finish_reason`
- 统一 usage 统计
- 统一 request id、latency、queue wait 日志
- 明确 HTTP 断开后的取消传播

### 12.3 需要收敛的内容

以下内容不再继续堆进 chat 主线：

- agent 专用字段作为主文档内容
- rag 调试字段作为主接口能力
- 过多与 chat 无关的 admin/debug 分支

### 12.4 兼容策略

短期内：

- 保留 `agent=true` 的兼容解析能力
- 但从主文档中移除
- 并逐步迁移到扩展区或 feature flag 下

## 13. `embeddings` 新增方案

### 13.1 接口目标

新增：

- `POST /v1/embeddings`

### 13.2 首期范围

首期仅支持：

- 单模型调用
- 文本输入
- 非流式返回

暂不追求：

- 多模态 embedding
- 批量超大输入优化
- 跨模型 fallback

### 13.3 平台行为

平台处理流程为：

1. 校验模型是否存在
2. 校验模型是否声明 `embeddings` capability
3. 路由到对应 backend
4. 返回标准 embedding 列表与 usage

### 13.4 backend 策略

- 若 local backend 具备 embedding 能力，则直接接入。
- 若 local 不具备而 stackflow 具备，则允许只在 stackflow 上声明。
- 若模型未声明该能力，则返回明确错误，而不是隐式 fallback 到 chat。

### 13.5 错误语义

建议新增统一错误码：

- `model_not_found`
- `capability_not_supported`
- `backend_not_available`
- `embedding_input_too_large`

## 14. `rerank` 新增方案

### 14.1 接口目标

新增：

- `POST /v1/rerank`

### 14.2 首期范围

首期聚焦标准文本 rerank：

- 一个 query
- 一组 documents
- 返回排序后结果与 score

### 14.3 平台行为

平台处理流程为：

1. 校验模型是否存在
2. 校验模型是否声明 `rerank` capability
3. 校验输入规模
4. 调用 backend
5. 返回排序结果

### 14.4 与 RAG 的关系

`rerank` 是正式模型能力，不等同于当前 RAG 调试接口。

二者关系应明确为：

- `rerank` 是正式模型 API
- `RAGExecutor` 是上层检索增强能力
- 前者进入平台主线
- 后者退出主线

## 15. agent 相关逻辑保留但退出主线

### 15.1 基本原则

agent 逻辑本次不直接删除，但要明确降级：

- 不再作为项目定位
- 不再作为主 README 核心卖点
- 不再占据 `chat/completions` 主叙事
- 不再驱动主接口设计

### 15.2 处理方式

建议按以下顺序处理：

- 保留 agent 代码目录与现有测试
- 将 agent 视为扩展能力或实验能力
- 在 API 层把 agent 入口与主线 handler 解耦
- 在文档层把 agent 放入“非主线/扩展能力”
- 后续通过配置项控制是否启用 agent 相关接口

### 15.3 兼容要求

为了避免一次性破坏现有功能：

- 短期不删 `AgentExecutor`
- 短期不删 `POST /v1/agent/debug`
- 短期不删 chat 中的 agent 兼容字段

但后续主线开发不再以这些能力为基础设计 API 和抽象。

## 16. 日志、错误码、超时、取消、限流、状态治理设计

### 16.1 日志

主线日志至少统一以下字段：

- `request_id`
- `api`
- `model`
- `backend`
- `capability`
- `session_id`
- `queue_wait_ms`
- `run_ms`
- `finish_reason`
- `status_code`
- `error_code`

要求：

- chat、embeddings、rerank 使用同一日志字段集合
- 错误日志与成功日志字段一致，便于检索和聚合

### 16.2 错误码

要建立平台统一错误码表，而不是继续由各 handler 临时拼接。

建议覆盖：

- 参数错误
- 模型不存在
- capability 不支持
- backend 不可用
- backend 超时
- 队列拥塞
- 客户端取消
- 内部执行异常

### 16.3 超时

超时至少分四层：

- HTTP 请求整体超时
- session/route queue 等待超时
- backend 执行超时
- stream idle 超时

当前已有的 `MAX_QUEUE_WAIT_MS`、`STACKFLOW_TIMEOUT_MS` 可以保留，但要纳入统一治理命名与日志。

当前已落地的 HTTP 侧治理配置为：

- `http_request_timeout_ms`
  对应环境变量 `HTTP_REQUEST_TIMEOUT_MS`，控制单个 HTTP 请求的总超时。
- `max_concurrent_requests`
  对应环境变量 `MAX_CONCURRENT_REQUESTS`，控制全局并发请求上限。
- `max_model_concurrency`
  对应环境变量 `MAX_MODEL_CONCURRENCY`，控制同一逻辑模型的并发请求上限。
- `max_session_concurrency`
  对应环境变量 `MAX_SESSION_CONCURRENCY`，控制同一 `session_id` 的并发请求上限。

### 16.4 取消

取消链路要形成闭环：

- HTTP 连接关闭
- API 层标记 request cancelled
- Service 层停止等待
- Backend 层尽可能停止执行
- 最终统一记录为 `cancelled`

### 16.5 限流

建议增加三类限流：

- 全局并发限制
- 按模型限制
- 按 session 或 client 限制

首期不要求复杂配额系统，但至少要支持静态配置限流和明确拒绝错误。

当前实现中三类静态限流均已进入 `HttpGateway` 的统一治理入口，拒绝时统一返回 `429`，并使用以下错误码：

- `rate_limit_global`
- `rate_limit_model`
- `rate_limit_session`

### 16.6 状态接口

主线只保留两个状态接口：

- `GET /admin/models/status`
- `GET /admin/backends/status`

建议语义如下：

`/admin/models/status` 返回：

- 模型列表
- 默认 backend
- gateway 视角默认 backend
- capability
- 是否已注册
- 当前可用性
- 已声明 / 当前可用 backend 列表
- 失败摘要

`/admin/backends/status` 返回：

- backend 类型
- gateway 视角 backend 名
- 连接状态
- 已加载 engine 数量
- 队列长度
- 最近错误
- 超时与取消统计
- 限流统计
- token 统计
- 当前平台 in-flight 请求数

当前实现已经对外暴露的状态 / 指标字段可直接作为阶段五基线：

- `/metrics`
  `requests_total`、`requests_in_flight`、`requests_stream_total`、`requests_error_total`、`requests_cancelled_total`、`requests_timeout_total`、`requests_rate_limited_total`、`prompt_tokens_total`、`completion_tokens_total`、`total_tokens_total`、`avg_latency_ms`
- `/admin/models/status`
  `registered`、`available`、`default_backend`、`gateway_default_backend`、`capabilities`、`declared_backends`、`available_backends`、`failure_summary`
- `/admin/backends/status`
  `backend`、`gateway_backend`、`connected`、`model_count`、`capabilities`、`loaded_engine_count`、`queue_length`、`requests_total`、`requests_error_total`、`requests_cancelled_total`、`requests_timeout_total`、`requests_rate_limited_total`、`last_error`、`timeout_total`、`cancelled_total`、`prompt_tokens_total`、`completion_tokens_total`、`total_tokens_total`、`requests_in_flight`

## 17. 建议目录演进

目标不是立即重排整个仓库，而是逐步形成更清晰边界。

建议主线目录向以下结构演进：

- `serving/http/`
- `serving/service/`
- `serving/backend/`
- `serving/core/`
- `serving/extensions/agent/`
- `serving/extensions/rag/`

迁移原则：

- `core` 放通用基础设施，如线程池、session、通用执行上下文、错误模型。
- `service` 放主线业务编排。
- `backend` 放 backend 接口与实现。
- `extensions` 放 agent/rag 之类非主线能力。

补充原则：

- 第一阶段优先建立逻辑边界，不强求立即把 `serving/core/agent/` 和 `serving/rag/` 物理搬迁到 `extensions/`。
- 只要主线新代码不再继续依赖 agent/rag 细节，就视为达成阶段目标。
- 目录迁移放在后续单独步骤完成，避免和主线平台骨架改造交叉放大风险。

## 18. 实施顺序与每步影响范围

本项目后续改造严格按以下顺序执行。

### 第 0 步：冻结主线目标并补本文档

目标：

- 确认平台定位
- 锁定主线接口范围

影响范围：

- `docs/`

风险：

- 无运行风险

### 第 1 步：建立最小平台骨架，不改 chat 外部行为

目标：

- 引入 capability 概念
- 引入平台级 request/response 与 service 骨架
- 建立模型目录和状态视图的基础结构
- 不改变现有 `/v1/chat/completions` 外部行为

主要影响范围：

- `serving/core/`
- `engine/`
- 新增 `serving/service/` 或 `serving/backend/`

阶段要求：

- chat 现有能力必须保持可用
- agent/rag 行为先不动
- 第一阶段不要求物理迁移 agent/rag 目录
- 第一阶段不要求重写现有 `ModelEngine::Run(ctx)` 实现

### 第 2 步：扩展模型注册表并补状态接口

目标：

- 扩展 `ModelRegistry`
- 支持 capability 声明
- 支持 backend 级能力差异
- 增加 `GET /healthz`
- 增加 `GET /admin/models/status`
- 增加 `GET /admin/backends/status`

主要影响范围：

- `engine/ModelRegistry.*`
- `serving/http/`
- `serving/service/`
- `config.json`
- 后续主文档

阶段要求：

- `/v1/models` 可返回 capability 信息
- 不破坏当前 local/rpc 选择逻辑
- `/health` 可短期保留为兼容别名
- admin 状态接口首期允许先返回静态声明 + 基础运行状态

### 第 3 步：拆分 API 层与 chat 主链路

目标：

- 缩减 `HttpGateway` 职责
- 让 chat 逻辑由 `ChatService` 承接
- 保持 SSE 与非流式兼容

主要影响范围：

- `serving/http/HttpGateway.*`
- `serving/http/ChatRequestParser.*`
- 新增 `serving/service/ChatService.*`

阶段要求：

- 现有 chat smoke test 必须继续通过
- chat 代码路径开始从 agent/rag 扩展逻辑中解耦
- 新增 chat 相关重构不能继续把 agent/rag 状态塞进主线 service

### 第 4 步：新增 `embeddings`

目标：

- 引入 embeddings request/response
- 打通模型注册表 capability 校验
- 至少接通一个 backend 实现

主要影响范围：

- `serving/http/`
- `serving/service/`
- `engine/` 或 `serving/backend/`
- `config.json`

阶段要求：

- 不支持 embeddings 的模型必须返回明确错误
- 至少一个 backend 真正可运行，不能只停留在空接口或 mock

### 第 5 步：新增 `rerank`

目标：

- 引入 rerank request/response
- 打通 capability 校验和 backend 调用

主要影响范围：

- `serving/http/`
- `serving/service/`
- `engine/` 或 `serving/backend/`
- `config.json`

阶段要求：

- 与 embeddings 同样走能力声明，不做硬编码分支
- 至少一个 backend 真正可运行，不能只返回占位响应

### 第 6 步：agent/rag 退出主线

目标：

- 从主 README、主文档、主 demo 中降级 agent/rag
- 把 agent/rag 迁移到扩展区或 feature flag

主要影响范围：

- `README.md`
- `docs/`
- `serving/http/`
- `serving/core/agent/`
- `serving/rag/`
- `demo/web/`

阶段要求：

- 保留兼容入口
- 但不再作为主产品面向
- 主 README 和主 demo 不再以 agent 为默认入口
- `docs/API调用示例.md` 和用户默认操作路径改为以 chat / models / embeddings / rerank 为主

### 第 7 步：统一治理能力

目标：

- 统一日志字段
- 统一错误码
- 补齐超时、取消、限流
- 清理散落的统计和状态实现

主要影响范围：

- `serving/http/`
- `serving/service/`
- `serving/core/`
- `engine/`

阶段要求：

- chat、embeddings、rerank 三条链路治理方式一致

## 19. 每一步改动的控制原则

为避免一次性大改，后续实施必须遵循以下规则：

- 每一步只做一个主题，不同时改抽象、接口和治理。
- 每一步都保持现有 chat 主链路可运行。
- 每一步都有明确影响范围和验证点。
- 非主线能力先降级、后隔离，最后再决定是否清理。
- 不先删 agent/rag，再补平台；应先把平台主线立住。

## 20. 验收标准

改造完成后，应满足以下标准：

- 项目主线心智清晰，定位为模型 API 平台而非 agent 平台。
- 主文档和主 demo 以 chat、embeddings、rerank 为中心。
- `ModelRegistry` 能表达模型 capability。
- `chat/completions` 稳定、兼容、可观测。
- `embeddings`、`rerank` 成为正式 API。
- 健康检查和状态接口统一。
- agent/rag 仍可保留，但已明确退出主线。

## 21. 结论

本次改造不是“删除 agent 功能”，而是“重建主线”。

主线应从“围绕 chat + agent + rag 混合演进”收敛为“围绕模型能力 API 演进”。

后续实施顺序必须固定为：

1. 文档定边界
2. 抽象先收口
3. chat 加固
4. embeddings / rerank 落地
5. agent/rag 降级
6. 治理补齐

在此顺序之外进行跳步式大改，不属于本次改造方案。
