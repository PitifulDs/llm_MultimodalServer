# 本地推理与 RPC 推理

本文档解释 EdgeLLM-Serving 中两种推理后端的区别：

- 本地推理：HTTP 服务进程直接加载 `llama.cpp` 与 GGUF 模型
- RPC 推理：HTTP 服务进程将请求转发给远端 `StackFlow` worker

---

## 1. 先区分两个“远程”

在实际使用中，经常会把下面两件事混在一起：

1. 浏览器是否在另一台电脑上访问 `http://<linux-server>:8080`
2. 模型是否由 HTTP 服务本机直接推理

这两个概念不是一回事。

例如：

- 你在 Windows 笔记本上打开 Web 页面，请求发到 Linux 服务器
- Linux 服务器上的 `serving_http_server` 直接加载 GGUF 并调用 `llama.cpp`

这种情况仍然是：

- 客户端访问是远程的
- 但模型推理是本地的

只有当 `serving_http_server` 把请求继续转发给远端 worker 时，才属于 RPC 推理。

---

## 2. 本地推理是什么

本地推理指：

- `serving_http_server`
- `HttpGateway`
- `EngineExecutor`
- `LlamaEngine`

最终都在同一台 Linux 机器上，模型由本机 `llama.cpp` 直接执行。

典型链路：

```text
Browser / curl
  -> serving_http_server
  -> HttpGateway
  -> EngineExecutor
  -> LlamaEngine
  -> llama.cpp
```

### 优势

- 链路短，延迟更低
- 结构简单，调试更直接
- 部署门槛低，单机即可运行
- 适合开发、自测、演示和单机场景

### 不足

- HTTP 服务和推理服务耦合在一起
- 模型加载、内存占用、CPU/GPU 资源都在同一进程体系附近
- 多机、多 worker、多模型扩展能力较弱

---

## 3. RPC 推理是什么

RPC 推理指：

- `serving_http_server` 不直接做模型推理
- 它通过 `StackFlowEngine` 将请求发给 `unit-manager`
- 再由远端 `node/test` worker 实际执行模型

典型链路：

```text
Browser / curl
  -> serving_http_server
  -> HttpGateway
  -> EngineExecutor
  -> StackFlowEngine
  -> unit-manager
  -> worker
  -> 模型执行
```

### 优势

- 网关层与推理解耦
- 可以把模型放到专门的计算节点上
- 更适合多 worker、多机扩容
- 更容易做资源隔离、故障切换和统一调度

### 不足

- 增加一跳 RPC 链路
- 延迟通常高于本地推理
- 部署与排障复杂度更高

---

## 4. 什么时候适合用本地推理

优先选择本地推理的情况：

- 单机开发
- 本地调试
- 演示环境
- 模型数量不多
- 没有单独的推理集群
- 追求更简单的部署和更低的延迟

对当前仓库的大多数开发场景，本地推理通常是默认选择。

---

## 5. 什么时候适合用 RPC 推理

优先选择 RPC 推理的情况：

- 希望 HTTP 网关和模型执行分离
- 模型运行节点与 API 节点不是同一台机器
- 需要多 worker 扩容
- 需要统一调度多个远端推理单元
- 需要把重资源模型放到独立机器或独立进程上

RPC 推理更偏向工程化和部署演进，不一定适合最早期单机阶段。

---

## 6. 当前项目里的使用方式

当前项目已经支持请求级后端选择：

```json
{
  "model": "qwen2.5-1.5b",
  "inference_backend": "local"
}
```

或：

```json
{
  "model": "qwen2.5-1.5b",
  "inference_backend": "rpc"
}
```

说明：

- `local` 表示本地 `llama.cpp`
- `rpc` 表示远端 `StackFlow` worker
- 同一个逻辑模型名可以根据开关切换后端

Web 页面中的 `Inference Backend` 下拉框就是这层请求级选择。

---

## 7. 一句话结论

本地推理解决“简单直接跑起来”，RPC 推理解决“把推理能力独立成可扩展的服务”。
