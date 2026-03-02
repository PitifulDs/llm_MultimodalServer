# 面试讲解稿（1 分钟 / 3 分钟 / 8 分钟）

## 1 分钟版本（电梯稿）
我做了一个 C++ 的 LLM Serving 系统，提供 OpenAI 兼容 API，支持非流式和 SSE 流式输出。后端统一封装本地 `llama.cpp` 和远程 `stackflow` 两种引擎。我主要负责 HTTP 网关、调度器、引擎接入和远程链路稳定性，解决了 TCP 分包、SSE 结束帧、UTF-8 断字符、远程超时等问题，并完成压测验证。

## 3 分钟版本（面试常用）
- 背景：需要一个可演示、可扩展的推理服务，不只跑本地模型，还要支持远程 worker。
- 方案：
  - `serving/http` 负责协议层（OpenAI chat + SSE）。
  - `serving/core` 负责调度和上下文。
  - `engine` 统一本地/远程推理实现。
- 我的关键工作：
  - 写了 HTTP 组包解析，避免 TCP 粘包/拆包导致 JSON 失败。
  - SSE 严格对齐 OpenAI chunk + `[DONE]`。
  - 远程链路加入 timeout、错误映射、复用串行保护。
  - 加入脚本与日志规范，支持快速排查。
- 结果：压测场景下 `llama` 与 `stackflow` 均达到稳定成功率。

## 8 分钟版本（深挖）
1. 架构分层与调用链（先讲 `README` 的结构图）。
2. 协议层关键点（HTTP 组包、SSE 结束、错误码）。
3. 调度层关键点（线程隔离、队列背压、finish 兜底）。
4. 引擎层关键点（本地采样参数、远程 setup/inference/exit）。
5. 典型故障复盘（timeout、UTF-8、空 delta）。
6. 指标与压测结果（稳定性、吞吐估算）。
7. 未来优化（P95/P99、监控、批处理、更完整测试）。

## 项目多线程模型（可直接讲）
- 一句话：这是一个“`Reactor IO 线程 + 业务线程池 + 两级串行队列`”的并发模型，目标是让网络收发不被推理阻塞，同时保证同一会话/同一模型的状态安全。
- 第一层（网络 IO 线程）：
  - `network::TcpServer` 使用 `EventLoopThreadPool` 做 Reactor 多路复用。
  - `setThreadNum(N)` 后，连接会按 round-robin 分配到不同 `EventLoop` 线程。
  - IO 线程只做收包、解包、回包，不做模型推理。
- 第二层（业务执行线程池）：
  - `HttpGateway` 内部持有 `ThreadPool pool_`，worker 数由 `WORKER_THREADS` 控制。
  - 请求进入后，把执行任务投递到业务线程，避免占用 EventLoop。
- 第三层（调度与背压）：
  - `SessionExecutor`：同一 `session` 串行执行（`pending + running`），防止多轮对话上下文并发写冲突。
  - `EngineExecutor`：按 `model` 维度排队（per-model queue），同模型任务串行消费。
  - 两级背压：`MAX_SESSION_PENDING` 控制会话队列上限，`MAX_MODEL_QUEUE` + `MAX_QUEUE_WAIT_MS` 控制模型队列容量和排队时延，超限直接失败返回。
- 同步与取消机制：
  - `ServingContext` 用 `atomic(cancelled/finished)` + `condition_variable` 协调非流式等待与流式结束。
  - 客户端断连时触发 `cancelled`，并通过 `EmitFinish(cancelled)` 让任务快速收敛，不继续无效写 socket。
- 引擎层并发保护：
  - `LlamaEngine`：访问会话 `history/model_ctx` 时加 `session->mu`，确保 KV cache 与历史更新一致。
  - `StackFlowEngine`：`work_id` 复用场景下用 `work_mu_` + `reuse_mu_` 做串行化，避免远程状态串扰。
- 后台线程：
  - `HttpGateway` 启动一个 Session GC 后台线程（每 60s 清理超时会话）。
  - `infra-controller` 的 `StackFlow` 有独立事件线程处理 RPC 事件队列。

## 面试可复述版本（30 秒）
- 我们把并发拆成三层：Reactor 负责网络、线程池负责执行、队列负责顺序和背压。
- 同一 session 串行，保证多轮上下文安全；同一 model 串行，避免引擎竞争。
- 超过队列上限或等待超时会快速失败，不把延迟扩散到全局。
- 断连会触发取消和 finish 兜底，避免线程空转和无效计算。

## 代码阅读速查（11 步）
1. 入口启动：`serving/http/http_main.cc`
   - `main`
   - `load_config`
   - `set_env_from_json`
2. HTTP 收包与路由：`serving/http/NetworkHttpServer.cc`
   - `onMessage`
   - `handleHttpRequest`
   - `onConnection`
3. 网关组装请求：`serving/http/HttpGateway.cc`
   - `HttpGateway::HttpGateway`
   - `HandleChatCompletion`
   - `HandleChatCompletionStream`
4. 会话串行调度：`serving/core/SessionExecutor.cc`
   - `Submit`
   - `Drain`
   - 配合看 `serving/core/Session.h` 的 `pending/running`
5. 模型队列调度：`serving/core/EngineExecutor.cc`
   - `Execute`
   - `SubmitPerModel`
   - `RunModelQueue`
6. 引擎工厂：`engine/EngineFactory.cc`
   - `Create`
   - `CreateNewEngine`
   - `ClearCache`
7. 本地推理（llama）：`engine/LlamaEngine.cc`
   - `Run`
   - `EnsureContext`
   - `CreateNewContext`
8. 远程推理（stackflow）：`engine/StackFlowEngine.cc`
   - `Run`
   - `ReadLine`
   - `SendLine`
9. 流式输出拼装：`serving/http/OpenAIStreamWriter.cc`
   - `OnChunk`
   - `split_utf8_prefix`
   - `finish_reason_to_str`
10. SSE 会话生命周期：`serving/http/HttpStreamSession.cc`
   - `Start`
   - `Write`
   - `Close`
11. 回包到网络线程：`serving/http/NetworkHttpTypes.h`
   - `Write`
   - `WriteInLoop`
   - `End`

## 高频追问的一句话答法
- 为什么要 OpenAI 兼容：上层接入成本最低。
- 为什么要 Reactor：避免 IO 线程被推理阻塞。
- 为什么远程更复杂：多进程、多状态、多网络跳数。
- 怎么保证稳定：超时、串行保护、日志关联、脚本化复现。
