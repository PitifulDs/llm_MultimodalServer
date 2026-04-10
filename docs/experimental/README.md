# Experimental

这里收纳默认关闭或只做“兼容保留”的能力（例如 agent / rag）。主线使用与对外展示不依赖这些能力。

启用方式通常有两层：

1. `config.json` 中的 `experimental_*_enabled`
2. 环境变量 `EXPERIMENTAL_*_API_ENABLED=1`（进程启动时读取；脚本也可能从 `config.json` 导出）

## Agent

- 使用说明（权威）：[`智能体使用说明.md`](智能体使用说明.md)
- 设计基线：[`分析智能体设计.md`](分析智能体设计.md)
- 旧调试说明（已合并）：[`调试说明.md`](调试说明.md)

## RAG

- 说明与入口：[`RAG说明.md`](RAG说明.md)
- v2 自测记录（阶段性）：[`../notes/rag_v2_self_test_2026-04-01.md`](../notes/rag_v2_self_test_2026-04-01.md)

