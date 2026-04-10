# Docs Refactor Plan (Applied)

本文档记录本次“从随手记到工程化文档体系”的重构落地结果：文件映射、合并/降级原因，以及后续维护建议。

## 1. 目录分层（新）

`docs/` 统一按四层分组：

- `docs/user-guide/`：如何用、如何运维（主线权威）
- `docs/architecture/`：系统如何工作、请求链路与模块职责（主线权威）
- `docs/experimental/`：默认关闭/兼容保留能力（agent/rag）
- `docs/notes/`：面试稿/亮点/压测/复盘/roadmap（沉淀但不进主链路）

## 2. old -> new 映射

根入口：

- `README.md` -> `README.md`（大幅收口；细节迁移到 `docs/`）

User Guide：

- `docs/API调用示例.md` -> `docs/user-guide/API调用示例.md`
- `docs/可观测性与排障.md` -> `docs/user-guide/可观测性与排障.md`
- new: `docs/user-guide/快速开始.md`
- new: `docs/user-guide/配置说明.md`
- new: `docs/user-guide/API接口.md`
- new: `docs/user-guide/治理与错误码.md`

Architecture：

- `docs/系统架构.md` -> `docs/architecture/系统架构.md`
- `docs/本地推理与RPC推理.md` -> `docs/architecture/本地推理与RPC推理.md`
- `docs/技术取舍.md` -> `docs/architecture/技术取舍.md`

Experimental：

- `docs/智能体使用说明.md` -> `docs/experimental/智能体使用说明.md`
- `docs/分析智能体设计.md` -> `docs/experimental/分析智能体设计.md`
- `docs/调试说明.md` -> `docs/experimental/调试说明.md`（内容合并，保留一个指向入口）
- new: `docs/experimental/RAG说明.md`

Notes：

- `docs/面试讲解稿.md` -> `docs/notes/面试讲解稿.md`
- `docs/面试问答.md` -> `docs/notes/面试问答.md`
- `docs/项目亮点.md` -> `docs/notes/项目亮点.md`
- `docs/设计模式.md` -> `docs/notes/设计模式.md`
- `docs/性能压测报告.md` -> `docs/notes/性能压测报告.md`
- `docs/问题复盘.md` -> `docs/notes/问题复盘.md`
- `docs/rag_v2_self_test_2026-04-01.md` -> `docs/notes/rag_v2_self_test_2026-04-01.md`
- `docs/模型API平台化改造文档.md` -> `docs/notes/模型API平台化改造文档.md`

索引页（新）：

- new: `docs/README.md`
- new: `docs/user-guide/README.md`
- new: `docs/architecture/README.md`
- new: `docs/experimental/README.md`
- new: `docs/notes/README.md`

## 3. 删除/合并原因（关键点）

- 根 `README.md` 从“堆料”收口到对外展示的一页：避免新人入口和面试入口被细节淹没。
- `API 调用示例` 不再重复列出完整错误码与统计口径：统一收敛到 `docs/user-guide/治理与错误码.md`。
- `系统架构` 不再重复列 API 清单与配置清单：分别引用 `docs/user-guide/API接口.md` 与 `docs/user-guide/配置说明.md`。
- `调试说明` 与 `智能体使用说明` 主题重叠：将可执行入口合并到 `智能体使用说明`，并保留 `调试说明` 作为兼容跳转，减少重复解释。
- 面试稿、亮点、压测、复盘、roadmap 全部下沉到 `docs/notes/`：避免主链路文档夹杂“演讲稿式内容”。

## 4. 后续维护建议

- 新增/变更主线接口：先改 `docs/user-guide/API接口.md`，再补 `docs/user-guide/API调用示例.md`，最后（如必要）补架构链路变化到 `docs/architecture/系统架构.md`。
- 新增/变更配置：只在 `docs/user-guide/配置说明.md` 做权威解释；其它文档只引用。
- agent/rag 这类默认关闭能力：统一放 `docs/experimental/`；主线文档只提示“默认关闭 + 链接”。
- 压测、事故复盘、阶段性自测：统一放 `docs/notes/`，并在 `docs/notes/README.md` 增补索引。
- 任何知识点如果出现三处以上重复，请合并到“最接近读者决策点”的那一处，其它位置改为链接。

