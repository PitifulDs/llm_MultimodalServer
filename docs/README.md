# Docs

这份文档体系按“使用者 / 维护者 / 实验特性 / 复盘沉淀”分层，目标是：

- 新人 10 分钟内知道从哪里开始看、怎么跑起来、怎么排障
- 面试官 5 分钟内看懂项目价值与工程亮点
- 同一知识点只在一个地方做“权威解释”，其它位置只做链接

## 快速入口

- 我想跑起来：[`user-guide/快速开始.md`](user-guide/快速开始.md)
- 我想看接口与示例：[`user-guide/API接口.md`](user-guide/API接口.md)，[`user-guide/API调用示例.md`](user-guide/API调用示例.md)
- 我想理解架构主线：[`architecture/系统架构.md`](architecture/系统架构.md)
- 我想把“接口实现”对上代码：[`architecture/接口实现映射.md`](architecture/接口实现映射.md)
- 我想理解双后端：[`architecture/本地推理与RPC推理.md`](architecture/本地推理与RPC推理.md)
- 我想看治理与排障：[`user-guide/治理与错误码.md`](user-guide/治理与错误码.md)，[`user-guide/可观测性与排障.md`](user-guide/可观测性与排障.md)

## 目录分层

- User Guide（如何用、如何运维）：[`user-guide/README.md`](user-guide/README.md)
- Architecture（系统怎么工作）：[`architecture/README.md`](architecture/README.md)
- Experimental（默认关闭的兼容/实验能力）：[`experimental/README.md`](experimental/README.md)
- Notes（面试稿、压测、复盘、roadmap）：[`notes/README.md`](notes/README.md)

## 维护约定

- 主线接口与配置说明以 `user-guide/` 为准。
- 请求链路与模块职责以 `architecture/` 为准。
- agent / rag 等默认关闭能力统一放在 `experimental/`，主文档只引用不展开。
- 面试稿、亮点、复盘、压测与阶段性记录统一沉到 `notes/`。

本次重构的文件映射与维护建议见：[`DOCS_REFACTOR_PLAN.md`](DOCS_REFACTOR_PLAN.md)。
