# 面试常见问题整理（EdgeLLM-Serving）

这份文档用于面试前快速复习。每个问题都给出简洁回答和可展开方向。

---

## 1. 项目总体与架构

1. 你这个项目是做什么的？
答: 一个端到端 LLM Serving 系统，提供 OpenAI 兼容 HTTP/SSE 接口，支持本地 llama.cpp 推理和 StackFlow 远程推理。
可展开: 为什么要双后端、如何切换、应用场景。

2. 系统的关键模块有哪些？
答: HTTP 接入层、调度层、推理引擎层、本地/远程 worker、底层网络模块。
可展开: 每层职责边界。

3. HTTP 到推理的完整链路是什么？
答: NetworkHttpServer 解析 HTTP → HttpGateway 校验与构建上下文 → EngineExecutor 调度 → LlamaEngine/StackFlowEngine 推理 → 返回 JSON 或 SSE。

4. 你为什么需要 StackFlow？
答: 让推理与 HTTP 服务解耦，支持多进程/远程扩展，避免推理占用 IO 线程。

---

## 2. HTTP 与 SSE

5. TCP 分包问题如何处理？
答: NetworkHttpServer 维护连接级 buffer，通过 Content-Length 判断 body 是否完整，避免 JSON 解析失败。

6. SSE 的输出格式怎么保证兼容？
答: OpenAIStreamWriter 统一输出 `data: {chunk}` 格式，完成时发送 `data: [DONE]`。

7. SSE 为什么要写 [DONE]？
答: 前端依赖结束标志释放状态，否则会一直处于 streaming。

---

## 3. 调度与并发

8. 为什么要 EngineExecutor？
答: 让 IO 线程与推理线程解耦，避免推理阻塞网络事件循环。

9. non-stream 请求如何等待完成？
答: 通过 on_finish 回调 + 条件变量等待，不阻塞网络线程。

10. 远程推理如何控制并发？
答: worker 内部 `try_begin_infer` 防止单进程并发；StackFlowEngine 支持 work_id 复用串行化。

---

## 4. 本地推理与远程推理

11. 本地推理和远程推理的差别？
答: 本地延迟低但占用 serving 资源；远程可扩展但增加网络与调度开销。

12. 远程推理请求流程是什么？
答: StackFlowEngine 发送 setup → inference → exit，通过 unit-manager 转发到 worker，并接收 token 流。

13. 为什么要 work_id 复用？
答: 避免每次请求重复加载模型，提高吞吐。

14. 复用会带来什么问题？
答: 并发请求可能冲突，所以要串行化或限制并发。

---

## 5. 参数与配置

15. 支持哪些采样参数？
答: max_tokens、temperature、top_p、top_k、repeat_penalty、presence_penalty、frequency_penalty、seed。

16. 参数从哪里传到引擎？
答: HttpGateway 解析 JSON，写入 ServingContext，最终由引擎使用。

17. 为什么要配置文件？
答: 方便切换模型路径、端口、超时、并发参数，避免硬编码。

---

## 6. 稳定性与排查

18. 遇到 500 错误怎么排查？
答: 先看 `/tmp/llm_serving/serving_http.log`，确认是 engine error、timeout 还是 setup 失败。

19. SSE 不结束怎么办？
答: 检查 OpenAIStreamWriter 是否输出 [DONE]，检查 on_finish 是否触发。

20. 第二次请求失败的原因？
答: work_id 复用导致的状态冲突或 socket 未清理，需启用串行化或清理 /tmp/llm/*.sock。

---

## 7. 设计选择与权衡

21. 为什么不把推理放在 HTTP 线程里？
答: 推理耗时长，会阻塞事件循环，导致连接超时或响应延迟。

22. 为什么自己做 HTTP 解析？
答: 需要精细控制 TCP 分包与 SSE，避免依赖过重。

23. 哪些地方是你做的？
答: Serving 层（HTTP、SSE、调度）、引擎接入与远程链路整合、worker 改造与脚本/配置文档。

---

## 8. 可扩展性

24. 如果要扩展到多 worker？
答: unit-manager 负责 worker 分配，worker 多开即可提升吞吐；必要时加入负载均衡策略。

25. 如果要支持多模型？
答: config 中加入模型映射，EngineFactory 依据 model 选择不同 GGUF 或 remote 通道。

---

## 9. 测试与指标

26. 有做过压测吗？
答: 使用脚本进行并发测试，观察平均延迟、P95 和错误率；结果记录在架构文档的指标段。

27. 如何验证 HTTP 解析正确？
答: 使用 curl + 分包场景测试，确认 JSON 解析稳定。

---

## 10. 后续改进

28. 你觉得还可以怎么优化？
答: Token batching、调度策略、更多指标监控、完善多轮聊天模板。

29. 如果有更多时间，你会做什么？
答: 做 Prometheus 指标、完善 health check、加入更严格的超时/重试策略。


---

## Reactor 相关（network 模块）

30. 什么是 Reactor？
答: 事件驱动模型，IO 多路复用 + 回调分发，应用在事件就绪时进行读写。

31. 你们项目里 Reactor 体现在哪些类？
答: `network/` 模块，`EventLoop` 负责事件循环，`Poller` 封装 epoll，`Channel` 绑定 fd 与回调，`TcpServer`/`TcpConnection` 管理连接。

32. Reactor 与 Proactor 区别？
答: Reactor 由应用在就绪后主动读写；Proactor 由系统完成 IO 后回调。

33. 为什么要非阻塞 IO？
答: 防止单连接阻塞事件循环，保证高并发。

34. 你们如何避免 IO 和推理互相阻塞？
答: IO 在 EventLoop 线程，推理在 EngineExecutor 的 worker 线程池。

35. epoll 的优势？
答: 大量连接场景下效率高，避免轮询全部 fd。

36. 适用场景？
答: 高并发、连接多、单次处理轻量的网络服务。



## 11. 深挖问题（高频追问）

37. 你们为什么选择 OpenAI 兼容协议？
答: 因为上层应用（Web、脚本、SDK）可以直接复用现有调用方式，迁移成本最低。底层可以替换本地模型或远端推理，不影响调用方。
可展开: 协议兼容不是模型兼容，兼容的是接口格式和行为（如 SSE + [DONE]）。

38. non-stream 和 stream 在服务端实现有什么本质差异？
答: non-stream 是“先算完再一次性返回”；stream 是“边生成边回写”。
可展开: stream 模式需要会话对象持续存活、处理客户端断连、按 chunk 发送并正确结束。

39. 为什么要做 TCP 组包解析，不能直接 `read` 一次就 parse 吗？
答: 不能。TCP 是字节流，不保证一次读到完整 HTTP 报文。必须用连接级 buffer + `Content-Length` 判定完整性。
可展开: 这是很多线上“偶发 JSON 解析失败”的根因。

40. 你的超时策略是如何设计的？
答: 分层超时：连接/读写超时、队列等待超时、推理执行超时。这样可以定位瓶颈在网络、调度还是模型推理。
可展开: 返回错误码时区分 `timeout`、`cancelled`、`internal_error`，便于监控告警。

41. 客户端中断（abort）后服务端怎么处理？
答: 通过取消标记让推理尽快停止，stream 结束时发清理逻辑，释放会话和资源，避免“僵尸任务”。
可展开: 这类场景在前端频繁点击 stop 时非常常见。

42. 为什么要把 IO 线程和推理线程分离？
答: 推理是重 CPU/GPU 任务，若在 IO 线程执行会拖慢所有连接，导致级联超时。
可展开: 这就是 Reactor + worker 线程池的经典分工。

43. 什么是 backpressure（背压），在你项目里怎么体现？
答: 背压是下游处理不过来时，上游限流或排队。你项目里体现为模型队列上限、会话 pending 上限、busy 拒绝。
可展开: 没有背压会导致内存膨胀和尾延迟恶化。

44. 为什么 stackflow 会出现“空 delta 立即 finish”的问题？
答: 根因是 worker 模型未正确加载，但链路仍返回完成帧，导致表面成功、实际无内容。
可展开: 修复点是模型路径解析/校验 + 引擎就绪检查，失败时显式报错而不是返回空结果。

45. 你是怎么定位这个问题的？
答: 对比 `serving_http.log` 与 `node_test.log`。HTTP 侧看到毫秒级 finish 且 token=0，worker 日志出现模型加载失败信息，最终定位到路径和加载逻辑。

46. 为什么相同模型本地和 stackflow 输出不一致？
答: 除模型权重外，还受 prompt 模板、角色拼接方式、采样参数、上下文复用策略影响。
可展开: 需要对齐 chat 模板、system/user/assistant 角色顺序和采样参数。

47. 如何解释“成功率高但质量差”的情况？
答: 工程成功率（HTTP 200）不等于模型质量。需要同时看内容质量指标（重复率、长度、关键词命中）和延迟/吞吐。

48. 你如何做故障分层？
答: 先判链路层（连接是否建立）→ 协议层（请求是否完整）→ 调度层（队列是否超时）→ 引擎层（模型是否就绪）。


## 12. 指标与压测（怎么讲得更专业）

49. 压测输出里的 `total/ok/failed/aborted` 分别代表什么？
答:
- `total`: 总请求数
- `ok`: 请求流程完成数（成功返回）
- `failed`: 失败请求数（超时/连接错误/500）
- `aborted`: 客户端主动中断数（压测脚本模拟）

50. `avg_dur_s` 和 `avg_bytes` 怎么解读？
答:
- `avg_dur_s`: 平均请求时长，越低越好
- `avg_bytes`: 平均返回体积，近似代表输出长度
可展开: 对比不同模型时，输出长度不同会直接影响时延。

51. 面试官问“吞吐是多少”怎么回答？
答: 可以给“估算吞吐”= 并发数 / 平均时延。
示例: `concurrency=2, avg_dur_s=2.199`，估算约 `0.91 req/s`。

52. 为什么要分“稳定场景”和“中断场景”压测？
答: 稳定场景看基础吞吐和时延；中断场景看取消逻辑、资源释放和系统鲁棒性。

53. 除了平均值，还应看什么？
答: P50/P95/P99、错误率、超时率、队列等待时间、TTFB（首字节时间）。
可展开: 平均值会掩盖长尾问题。

54. 你项目当前压测可结论是什么？
答: llama 链路稳定（30/30、80/80）；stackflow 在修复模型路径后稳定场景 20/20 成功，说明远程链路可用。


## 13. StackFlow / RPC / ZMQ 追问

55. 这里说的 RPC 是什么？
答: RPC（Remote Procedure Call，远程过程调用）就是“像调用本地函数一样调用远端服务”。
在项目里，HTTP 服务通过 RPC 请求 unit-manager/worker 执行 setup/inference/exit。

56. 为什么同时用了 RPC 和 ZMQ？
答: RPC 负责“请求-响应”控制路径，ZMQ 负责流式事件分发和消息传输。
可展开: 控制面和数据面分离更清晰。

57. ZMQ 常见通信模式有哪些？
答: Req/Rep（请求响应）、Pub/Sub（发布订阅）、Push/Pull（任务分发）。
你项目中流式输出更接近 Pub/Sub 语义。

58. setup/inference/exit 三段式有什么好处？
答: setup 做资源准备，inference 做实际生成，exit 做资源清理。便于复用、限流和故障恢复。

59. work_id 有什么作用？
答: work_id 是远程 worker 侧的会话/实例标识，用于复用状态、路由消息、释放资源。

60. 什么情况下要关闭 work_id 复用？
答: 当并发冲突或状态串扰难以控制时。优先保证正确性，再做复用优化。


## 14. 采样参数（名词 + 作用）

61. temperature 是什么？
答: 温度系数，控制随机性。越高越发散，越低越确定。

62. top_p 是什么？
答: 核采样（nucleus sampling），只在累计概率前 p 的候选里采样。

63. top_k 是什么？
答: 只在概率最高的 k 个 token 中采样。

64. repeat_penalty 是什么？
答: 对已生成 token 做惩罚，减少重复。

65. presence/frequency penalty 区别？
答: presence 偏“是否出现过”；frequency 偏“出现次数”。两者都可抑制重复，但侧重点不同。

66. seed 的意义？
答: 随机种子。固定 seed 可以让采样结果更可复现，方便回归测试。


## 15. 名词解释（面试常用术语）

- OpenAI 兼容: 指 API 协议兼容（路径、字段、SSE 行为），不代表使用 OpenAI 模型。
- SSE: Server-Sent Events，服务端单向流式推送协议，基于 HTTP。
- TTFB: Time To First Byte，从发请求到收到第一个字节的时间。
- Tail Latency: 尾延迟，常看 P95/P99，反映最慢那部分请求。
- Reactor: 事件驱动模型，IO 就绪后由应用回调处理。
- EventLoop: 事件循环，负责轮询 fd 并分发回调。
- epoll: Linux 高性能 IO 多路复用机制。
- Backpressure: 背压机制，下游忙时上游限流/排队。
- Idempotent（幂等）: 重复执行多次结果一致。
- Chat Template: 把多轮消息按模型要求拼接成 prompt 的模板。
- KV Cache: Transformer 推理时缓存历史 Key/Value，减少重复计算。
- Warmup: 服务启动后先做一次预热，减少首请求抖动。
- Control Plane/Data Plane: 控制面负责调度治理，数据面负责实际传输。


## 16. 面试时的“回答结构模板”（建议）

67. 如果被问“你如何解决一个线上问题？”
答题结构建议:
1) 现象: 明确用户可见问题（如 200 但无内容）
2) 定位: 给出日志证据和分层排查路径
3) 根因: 指向具体代码或配置
4) 修复: 说明改动点与回归验证
5) 结果: 用数据收尾（成功率、时延）

68. 如果被问“你的贡献是什么？”
答题结构建议:
1) 负责范围（HTTP 网关→调度→引擎）
2) 关键难点（分包、SSE、远程链路）
3) 具体改动（文件级）
4) 可量化结果（压测、稳定性）
