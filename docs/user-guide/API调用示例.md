# 接口说明与调用示例

默认服务地址：`http://localhost:8080`

接口范围与字段语义见：[`API接口.md`](API接口.md)。

错误码、metrics/admin status 统计口径见：[`治理与错误码.md`](治理与错误码.md)。

## 1. 健康检查
```bash
curl -s http://localhost:8080/healthz | jq
```

兼容别名：
```bash
curl -s http://localhost:8080/health | jq
```

## 2. 模型目录
```bash
curl -s "http://localhost:8080/v1/models" | jq
```

重点字段：
```json
{
  "id": "qwen3.5-2b",
  "default_backend": "local",
  "backends": ["local"],
  "capabilities": ["chat", "embeddings", "rerank"]
}
```

说明：
- `backends` 是该模型实际声明可用的后端；未声明 `rpc` 的模型，请求 `inference_backend:"rpc"` 会明确失败。
- `capabilities` 是调用方可依赖的模型能力；当前 `embeddings/rerank` 只通过本地 `local` 后端实现。

## 3. 非流式 chat
```bash
curl -s -X POST "http://localhost:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "inference_backend":"local",
    "messages":[{"role":"user","content":"hello"}]
  }' | jq
```

请求格式：
```json
{
  "model": "qwen3.5-2b",
  "inference_backend": "local",
  "messages": [
    {"role": "user", "content": "hello"}
  ],
  "max_tokens": 128
}
```

响应格式：
```json
{
  "id": "chatcmpl-req-1",
  "object": "chat.completion",
  "model": "qwen3.5-2b",
  "choices": [
    {
      "index": 0,
      "message": {"role": "assistant", "content": "..."},
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 10,
    "completion_tokens": 20,
    "total_tokens": 30
  }
}
```

## 4. 流式 SSE chat
```bash
curl -N -X POST "http://localhost:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "stream":true,
    "messages":[{"role":"user","content":"介绍下华为"}],
    "max_tokens":128
  }'
```

## 5. 带采样参数的 chat
```bash
curl -s -X POST "http://localhost:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "messages":[{"role":"user","content":"写一个快速排序"}],
    "max_tokens":200,
    "temperature":0.7,
    "top_p":0.9,
    "top_k":40,
    "repeat_penalty":1.1
  }' | jq
```

## 6. embeddings
```bash
curl -s -X POST "http://localhost:8080/v1/embeddings" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "input":"hello embeddings"
  }' | jq
```

请求格式：
```json
{
  "model": "qwen3.5-2b",
  "input": "hello embeddings"
}
```

响应格式：
```json
{
  "object": "list",
  "model": "qwen3.5-2b",
  "data": [
    {
      "object": "embedding",
      "index": 0,
      "embedding": [0.01, 0.02]
    }
  ],
  "usage": {
    "prompt_tokens": 2,
    "total_tokens": 2
  }
}
```

## 7. rerank
```bash
curl -s -X POST "http://localhost:8080/v1/rerank" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "query":"hello rerank",
    "documents":[
      "totally unrelated weather report",
      "hello rerank"
    ],
    "top_n":1
  }' | jq
```

请求格式：
```json
{
  "model": "qwen3.5-2b",
  "query": "hello rerank",
  "documents": [
    "totally unrelated weather report",
    "hello rerank"
  ],
  "top_n": 1
}
```

响应格式：
```json
{
  "object": "list",
  "model": "qwen3.5-2b",
  "data": [
    {
      "object": "rerank_result",
      "index": 1,
      "document": "hello rerank",
      "relevance_score": 0.9
    }
  ],
  "usage": {
    "prompt_tokens": 4,
    "total_tokens": 4
  }
}
```

## 8. admin 状态接口
```bash
curl -s "http://localhost:8080/admin/models/status" | jq
curl -s "http://localhost:8080/admin/backends/status" | jq
```

## 9. 错误请求示例
```bash
curl -i -s -X POST "http://localhost:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3.5-2b","messages":"bad"}'
```

预期：`HTTP 400`。

## 10. 远程后端示例
```bash
curl -s -X POST "http://localhost:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "inference_backend":"rpc",
    "messages":[{"role":"user","content":"hello from remote"}]
  }' | jq
```

备注：
- 请求字段 `inference_backend` 支持 `local` 或 `rpc`，其中 `rpc/remote/worker/stackflow` 会归一为内部 `stackflow`。
- 如果模型没有在 `config.json` 声明 `rpc/stackflow` backend，请求 `inference_backend:"rpc"` 会返回 `backend_not_available`，不会自动 fallback。
- 当前 `embeddings/rerank` 只通过本地 `LlamaEngine` 实现，不会向调用方暴露未实现的 RPC 能力。
- 远程 `stackflow` chat 返回里的 `usage` 当前为近似值。

未声明 RPC backend 的失败示例：
```json
{
  "error": {
    "code": "backend_not_available",
    "message": "requested backend is not declared or does not support capability for model: qwen3.5-2b",
    "type": "invalid_request_error"
  }
}
```

## 11. 治理示例
超时：
```bash
HTTP_REQUEST_TIMEOUT_MS=1 ./build/serving/http/serving_http_server
curl -i -s -X POST "http://localhost:8080/v1/embeddings" \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3.5-2b","input":"hello embeddings"}'
```

预期：`HTTP 504`，错误码 `request_timeout`。

限流：
```bash
curl -s "http://localhost:8080/metrics" | jq
curl -s "http://localhost:8080/admin/backends/status" | jq
```

关注字段：
- `requests_error_total`
- `requests_cancelled_total`
- `requests_timeout_total`
- `requests_rate_limited_total`
- `last_error`

## 12. 主线自检脚本
```bash
bash scripts/smoke_test.sh
```

更轻量的 Python API 自检脚本：
```bash
scripts/api_smoke_test.py
```

指定地址或模型：
```bash
BASE_URL=http://localhost:18080 MODEL=qwen3.5-2b scripts/api_smoke_test.py
```

如果刚后台启动服务后立即测试，脚本会在首次 `/v1/models` 请求上自动重试等待服务监听。可通过以下变量调整等待：
```bash
CONNECT_RETRIES=60 CONNECT_RETRY_INTERVAL=0.5 scripts/api_smoke_test.py
```

## 13. 扩展能力与兼容入口

agent/rag 仍保留兼容能力，但不再作为默认 API 路径：
- `POST /v1/chat/completions` 仍兼容 `agent=true` 与 `rag` 扩展字段。
- `POST /v1/agent/debug`、`POST /v1/retrieval/search`、`GET /admin/rag/status`、`POST /admin/rag/reload-index` 默认关闭。
- 如需重新开启，显式设置：
  - `EXPERIMENTAL_AGENT_API_ENABLED=1`
  - `EXPERIMENTAL_RAG_API_ENABLED=1`

扩展验证脚本：
```bash
bash scripts/smoke_test_agent_code_analysis.sh
bash scripts/smoke_test_agent_web_research.sh
bash scripts/smoke_test_rag.sh
bash scripts/smoke_test_rag_v2.sh
```
