# 项目亮点（面试开场版）

## 1. 一句话介绍
我做了一个 OpenAI 兼容的 LLM 推理服务系统，支持本地 `llama.cpp` 和远程 `stackflow` 双后端，提供非流式和 SSE 流式接口，并补齐了稳定性、可观测和脚本化运维能力。

## 2. 我负责的核心工作
- 打通 `HTTP Gateway -> EngineExecutor -> Llama/StackFlow Engine` 主链路。
- 实现 TCP 组包解析、SSE `data: ...` 与 `[DONE]` 结束帧对齐。
- 完成远程链路 setup/inference/exit 协议接入与 timeout/错误处理。
- 设计启动/停止脚本与日志规范，支持快速复现和排障。
- 增加单元测试（HTTP 参数与工具函数）提升可维护性。

## 3. 可量化结果（当前版本）
- `llama` 压测：
  - `concurrency=6, rounds=30`，`ok=30/30`
  - `concurrency=10, rounds=80, abort_ratio=0.4`，`ok=80/80`
- `stackflow` 稳定场景：
  - `concurrency=2, rounds=20`，`ok=20/20`

## 4. 面试开场（30 秒模板）
这个项目是一个 C++ 实现的 LLM Serving 系统，目标是把本地和远程推理统一在一套 OpenAI 兼容 API 下。我主要做了三件事：第一是 HTTP/SSE 协议层工程化，解决 TCP 分包和流式结束对齐；第二是调度与引擎解耦，让本地 `llama.cpp` 和远程 `stackflow` 可以平滑切换；第三是稳定性建设，包括超时、日志、脚本和单元测试。最后我做了压测，`llama` 和 `stackflow` 都跑通了稳定场景。
