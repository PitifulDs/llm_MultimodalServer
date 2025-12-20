# HTTP Gateway（Serving v2）

本模块是 Edge-LLM-Infra 的 **北向接入适配层（Northbound Adapter）**。

它的唯一作用是：
- 接收外部 HTTP / SSE 请求（curl / Python / Web 等）
- 将 HTTP 请求 **转换为 StackFlows 的 RPC / 事件**
- 将 StackFlows 的流式事件 **转发回 HTTP 客户端**

---

## 一、模块职责（必须遵守）

### 本模块【只允许】做的事情

- HTTP / SSE 协议解析与响应
- 请求参数校验与基础转换
- HTTP 请求 → StackFlows RPC / Event 封装
- StackFlows PUB/SUB 事件 → HTTP 流式输出
- request_id / session_id 的透传与映射

---

## 二、模块边界（禁止事项）

本模块 **严禁** 出现以下行为或依赖：

- ❌ 不包含任何推理逻辑
- ❌ 不包含 unit-manager 相关逻辑
- ❌ 不直接访问 Node / Task
- ❌ 不加载或操作模型（如 llama.cpp）
- ❌ 不管理 session 生命周期
- ❌ 不进行调度、路由或资源分配

所有 **调度 / Session / KV Cache / 推理执行**  
必须由 StackFlows 与 unit-manager 负责。

---

## 三、设计原则

- 本模块必须保持 **无状态**
- 本模块是 **协议适配层，而非服务核心**
- HTTP Gateway 只是 StackFlows 的一个客户端
- 所有外部请求必须通过 StackFlows 进入系统

---

## 四、架构定位说明

在整体架构中，本模块位于：
---
    外部用户（HTTP）
        ↓
    HTTP Gateway（本模块）
        ↓
    StackFlows（ZMQ / RPC / Flow）
        ↓
    unit-manager
        ↓
    Node / Task / Model
---
## 五、重要说明（给未来维护者）

如果你发现自己想在这里：
- 加模型推理
- 加 unit 选择逻辑
- 加 session 管理

**说明设计方向已经错了，请立刻回退。**