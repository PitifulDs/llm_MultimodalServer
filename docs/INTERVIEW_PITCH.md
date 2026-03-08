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

## 项目里用到的大模型知识
- `Chat Template / Prompt 拼接`
  - 多轮 `messages` 不是直接拼字符串，而是按模型模板转成 prompt，并且只取本轮增量部分。
- `Tokenization / Detokenization`
  - 文本先转 token，再把生成的 token 转回文本片段输出。
- `Prefill + Decode`
  - 先把 prompt 做 prefill，再逐 token decode，这是标准自回归推理流程。
- `KV Cache / 多轮续写`
  - 会话级保存 `model_ctx + n_past`，后续请求复用上下文，减少重复计算。
- `会话记忆与增量 diff`
  - `Session.history` 保存多轮历史；如果新请求是旧历史的前缀扩展，只把新增消息送进模型。
- `上下文窗口管理`
  - 用 `n_ctx` 和 `KV_RESET_MARGIN` 做溢出保护，接近上限就重建上下文。
- `采样参数`
  - 项目打通了 `max_tokens`、`temperature`、`top_p`、`top_k`、`repeat_penalty`、`presence_penalty`、`frequency_penalty`、`seed`。
- `停止条件与 finish reason`
  - 识别结束 token，并区分 `stop / length / cancelled / error`。
- `流式生成（SSE）`
  - 内部按 delta/token 回调，再适配成 OpenAI 风格 SSE chunk 和 `[DONE]`。
- `取消生成`
  - 客户端断开或任务取消时，生成流程会尽快停止，避免无效计算。
- `本地/远程双后端统一抽象`
  - 本地 `llama.cpp` 和远程 `stackflow` 都统一到 `ServingContext -> ModelEngine::Run`。
- `远程模型实例复用`
  - `work_id` 本质上是远程模型实例/上下文标识，复用能减少 setup 成本，但要串行化防止状态串扰。
- `会话持久化`
  - 现在支持把 `Session.history` 落 Redis，进程重启后恢复多轮历史。

## 面试可复述版本（LLM 知识）
- 这个项目不只是“调模型 API”，而是把 `chat template`、tokenize/detokenize、prefill/decode、KV cache、多轮记忆、采样参数、流式生成、上下文窗口管理这些大模型核心知识，真正落到了服务端实现里。

## 名词解释速记
- `Prompt`
  - 喂给模型的输入文本。
- `Chat Template`
  - 把 `system/user/assistant` 多轮消息按模型要求格式化成 prompt 的模板。
- `Token`
  - 模型处理的最小文本单元，不一定等于一个汉字或一个单词。
- `Tokenization`
  - 把文本切成 token 的过程。
- `Detokenization`
  - 把 token 再还原成可读文本的过程。
- `Prefill`
  - 先把整段 prompt 输入模型，计算出初始上下文状态。
- `Decode`
  - 在已有上下文基础上，一步一步生成下一个 token。
- `自回归生成`
  - 每次生成一个 token，再把它作为下一步输入的一部分继续生成。
- `KV Cache`
  - Transformer 推理时缓存历史 token 的 Key/Value，避免每轮都从头计算。
- `n_past`
  - 当前已经写入 KV Cache 的 token 数。
- `上下文窗口 / Context Window`
  - 模型一次最多能看到的 token 长度上限。
- `n_ctx`
  - 上下文窗口大小配置。
- `Sampling`
  - 按概率而不是固定贪心地选择下一个 token。
- `temperature`
  - 温度，越高越随机，越低越保守。
- `top_k`
  - 只在概率最高的前 `k` 个 token 里采样。
- `top_p`
  - 只在累计概率达到阈值 `p` 的候选 token 集合里采样。
- `repeat_penalty`
  - 对已生成过的 token 加惩罚，减少重复。
- `presence_penalty`
  - 某个词只要出现过，再出现就会被惩罚。
- `frequency_penalty`
  - 某个词出现越多，后续惩罚越大。
- `seed`
  - 随机种子，用来让采样结果更可复现。
- `EOG / EOS`
  - 生成结束 token，模型输出到这里就停止。
- `Finish Reason`
  - 一次生成结束的原因，比如 `stop`、`length`、`cancelled`、`error`。
- `SSE`
  - `Server-Sent Events`，服务端通过 HTTP 持续单向推送流式结果。
- `Delta`
  - 流式输出里每次新增的一小段文本。
- `Session`
  - 一次多轮对话会话，对应一份历史和可能复用的模型上下文。
- `Session History`
  - 当前会话里累计的历史消息。
- `Auto Diff`
  - 比较旧历史和新请求，只把新增消息送给模型。
- `Model Context`
  - 模型运行时上下文，通常包括 KV Cache、采样器等状态。
- `work_id`
  - 远程 worker 侧的实例/上下文标识，用于复用和路由。
- `OpenAI 兼容`
  - 兼容的是 API 协议和返回格式，不代表底层一定是 OpenAI 模型。

## 项目里智能指针的用法
- `unique_ptr`
  - 用来管理独占资源，只有一个主人，跟宿主对象一起创建和销毁。
  - 项目里典型是 `HttpGateway` 独占 `SessionManager`，以及网络层/控制层独占底层句柄、线程对象。
- `shared_ptr`
  - 用来解决跨模块、跨线程、跨回调的生命周期问题，多个地方都可能还在用这个对象。
  - 项目里典型是 `ServingContext`、`Session`、`ModelEngine`、`HttpStreamSession`、`NetworkHttpResponse`。
  - 比如一次流式请求会经过 `HttpGateway -> SessionExecutor -> EngineExecutor -> Engine -> SSE 回写`，对象会被多层 lambda 和异步任务捕获，用 `shared_ptr` 才能保证它在链路结束前不被提前销毁。
- `weak_ptr`
  - 表示“我引用你，但我不拥有你”，常用于回调、防循环引用、防悬挂访问。
  - 项目里 `node/test` 的回调先拿 `weak_ptr`，执行时再 `lock()`；对象已经释放就直接返回，避免回调晚到导致野指针。
- `enable_shared_from_this`
  - 用在对象内部安全拿到自己的 `shared_ptr`。
  - 这个项目里最典型的是异步写回：`NetworkHttpResponse::Write/End` 如果不在 IO 线程，会先 `shared_from_this()` 抓一份 `self`，再把任务投递到 `queueInLoop`，这样回调真正执行前对象不会析构。
  - `HttpStreamSession` 也一样，`Start()` 时保留一份 `self_`，流结束 `Close()` 再释放，保证整个 SSE 生命周期稳定。

## 面试可复述版本（智能指针）
- 这个项目里智能指针不是为了“语法现代”，而是明确表达生命周期语义：`unique_ptr` 管独占资源，`shared_ptr` 管异步链路生命周期，`weak_ptr` 防回调悬挂，`shared_from_this` 解决跨线程回调里的对象保活。

## 高频追问的一句话答法
- 为什么要 OpenAI 兼容：上层接入成本最低。
- 为什么要 Reactor：避免 IO 线程被推理阻塞。
- 为什么远程更复杂：多进程、多状态、多网络跳数。
- 怎么保证稳定：超时、串行保护、日志关联、脚本化复现。
