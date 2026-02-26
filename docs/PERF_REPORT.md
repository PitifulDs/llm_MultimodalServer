# 压测与性能报告（简版）

## 1. 环境与方法
- 压测脚本：`sample/stress_sse.py`
- 模式：SSE 流式
- 指标：`total/ok/failed/aborted/avg_dur_s/avg_bytes`

吞吐估算：
- 近似 `QPS = 并发数 / avg_dur_s`

## 2. 当前结果

### llama
- 场景 A：`concurrency=6, rounds=30, abort_ratio=0`
  - `ok=30/30`，`avg_dur_s=15.437`
  - 估算吞吐约 `0.39 req/s`
- 场景 B：`concurrency=10, rounds=80, abort_ratio=0.4`
  - `ok=80/80`，`aborted=33`，`avg_dur_s=15.907`
  - 估算吞吐约 `0.63 req/s`

### stackflow
- 场景：`concurrency=2, rounds=20, abort_ratio=0`
  - `ok=20/20`，`avg_dur_s=2.199`
  - 估算吞吐约 `0.91 req/s`

## 3. 结论
- 当前版本在 demo 规模下稳定性可用。
- stackflow 在稳定场景吞吐更高，但链路复杂度也更高。

## 4. 下一步建议
- 增加 `P50/P95/P99` 与 TTFB 统计。
- 区分网络排队时间与模型推理时间。
- 增加固定提示词集合，做内容质量一致性对比。
