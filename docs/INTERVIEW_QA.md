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

