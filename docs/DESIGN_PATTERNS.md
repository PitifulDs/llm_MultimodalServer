# 设计模式落地说明（EdgeLLM-Serving）

这份文档只讲“项目里真实用到的模式”，并给出对应代码位置和可面试复述的示例。

---

## 1. 工厂模式（Factory）+ 缓存复用

**解决的问题**
- 按 `model` 动态创建不同引擎（`llama` / `stackflow` / `dummy`）。
- 避免重复创建昂贵对象（模型加载成本高）。

**落地位置**
- `engine/EngineFactory.cc`
- `engine/EngineFactory.h`

**核心点**
- `CreateNewEngine(model)` 负责“创建策略”。
- `Create(model)` 负责“缓存命中 + 双检写回”。

**示例（简化）**
```cpp
std::shared_ptr<ModelEngine> EngineFactory::Create(const std::string &model) {
    { // 先读缓存
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_cache.find(model);
        if (it != g_cache.end()) return it->second;
    }

    auto eng = CreateNewEngine(model); // 锁外创建，避免长时间占锁
    if (!eng) return nullptr;

    std::lock_guard<std::mutex> lk(g_mu);
    auto &slot = g_cache[model];
    if (!slot) slot = eng;
    return slot;
}
```

---

## 2. 策略模式（Strategy，靠多态实现）

**解决的问题**
- 上层调度不关心“本地推理还是远程推理”，统一调用入口。

**落地位置**
- 抽象接口：`serving/core/ModelEngine.h`
- 具体策略：`engine/LlamaEngine.cc`、`engine/StackFlowEngine.cc`、`engine/RpcEngine.cc`

**核心点**
- `ModelEngine::Run(ctx)` 是统一策略接口。
- `EngineExecutor` 只依赖抽象，不依赖具体实现细节。

**示例（简化）**
```cpp
class ModelEngine {
public:
    virtual ~ModelEngine() = default;
    virtual void Run(std::shared_ptr<ServingContext> ctx) = 0;
};

// 调用侧
auto engine = EngineFactory::Create(ctx->model);
engine->Run(ctx);
```

---

## 3. 适配器模式（Adapter）

**解决的问题**
- 内部流式结构不是 OpenAI 原生格式，需要适配成标准 SSE chunk。

**落地位置**
- `serving/http/OpenAIStreamWriter.cc`

**核心点**
- 输入：内部 `StreamChunk`（`delta/is_finished/finish_reason`）
- 输出：OpenAI SSE：
  - `data: {"choices":[{"delta":{"content":"..."}}]}`
  - `data: [DONE]`

**示例（简化）**
```cpp
void OpenAIStreamWriter::OnChunk(const StreamChunk &chunk) {
    json j;
    j["object"] = "chat.completion.chunk";
    if (!chunk.is_finished) {
        j["choices"] = {{{"delta", {{"content", chunk.delta}}}, {"finish_reason", nullptr}}};
    } else {
        j["choices"] = {{{"delta", json::object()}, {"finish_reason", "stop"}}};
    }
    write_("data: " + j.dump() + "\\n\\n");
    if (chunk.is_finished) write_("data: [DONE]\\n\\n");
}
```

---

## 4. Reactor 模式（网络事件驱动）

**解决的问题**
- 高并发连接下避免“一个连接阻塞全局”。

**落地位置**
- `network/src/EventLoop.cc`
- `network/src/Poller.cc`
- `network/src/Channel.cc`

**核心点**
- `Poller` 用 `epoll_wait` 等待事件。
- `Channel` 绑定 fd 与读写回调。
- `EventLoop` 主循环分发就绪事件。

**示例（简化）**
```cpp
while (!quit_) {
    activeChannels_.clear();
    poller_->poll(kPollTimeMs, &activeChannels_);
    for (Channel *ch : activeChannels_) {
        ch->handleEvent(); // 回调 read/write/error
    }
    doPendingFunctors();
}
```

---

## 5. 生产者-消费者（Producer-Consumer）+ 串行队列

**解决的问题**
- HTTP 线程只负责接入，推理在线程池执行。
- 同一模型可配置排队与背压（队列满拒绝）。

**落地位置**
- `serving/core/EngineExecutor.cc`

**核心点**
- `SubmitPerModel` 把任务放进对应模型队列。
- `RunModelQueue` 在线程池中串行消费。

**示例（简化）**
```cpp
bool EngineExecutor::SubmitPerModel(...) {
    if (mq->tasks.size() >= cap) return false; // backpressure
    mq->tasks.push_back(task);
    if (!mq->running) {
        mq->running = true;
        pool_.Submit([this, mq]{ RunModelQueue(..., mq); });
    }
    return true;
}
```

---

## 6. 观察者/回调模式（Observer via Callback）

**解决的问题**
- 引擎层和 HTTP 输出层解耦：引擎只发事件，不直接写网络。

**落地位置**
- `serving/core/ServingContext.h`
- 回调绑定方：`serving/http/HttpGateway.cc`

**核心点**
- `on_chunk`：流式增量回调。
- `on_finish`：结束事件回调（统一收口）。

**示例（简化）**
```cpp
ctx->on_chunk = [&](const StreamChunk &c) { writer.OnChunk(c); };
ctx->on_finish = [&](FinishReason r) { /* 记录指标、会话收尾 */ };

// 引擎侧
ctx->EmitDelta(token_text);
ctx->EmitFinish(FinishReason::stop);
```

---

## 7. 为什么这些模式重要（面试回答模板）

- **Factory + Strategy**：方便扩展新引擎（比如再加 `TensorRTEngine`），不改网关主流程。
- **Reactor + 线程池**：保证网络层不被推理阻塞。
- **Adapter**：保证对外协议稳定（OpenAI 兼容），内部实现可演进。
- **回调解耦**：引擎不依赖 HTTP，便于未来增加 gRPC/WebSocket 输出。

---

## 8. 反例（你可以主动说明的工程判断）

- 这个项目**没有强行套用**单例/模板方法等模式；优先保证链路稳定和可维护。
- 设计模式是为了解决具体问题，不是为了“凑名词”。
