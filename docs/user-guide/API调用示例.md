# 接口说明与调用示例

默认服务地址：`http://127.0.0.1:8080`

接口范围与字段语义见：[`API接口.md`](API接口.md)。

错误码、metrics/admin status 统计口径见：[`治理与错误码.md`](治理与错误码.md)。

## 1. 健康检查
```bash
curl -s http://127.0.0.1:8080/healthz | jq
```

兼容别名：
```bash
curl -s http://127.0.0.1:8080/health | jq
```

## 2. 模型目录
```bash
curl -s "http://127.0.0.1:8080/v1/models" | jq
```

## 3. 非流式 chat
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "messages":[{"role":"user","content":"hello"}]
  }' | jq
```

## 4. 流式 SSE chat
```bash
curl -N -X POST "http://127.0.0.1:8080/v1/chat/completions" \
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
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
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
curl -s -X POST "http://127.0.0.1:8080/v1/embeddings" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "input":"hello embeddings"
  }' | jq
```

## 7. rerank
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/rerank" \
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

## 8. admin 状态接口
```bash
curl -s "http://127.0.0.1:8080/admin/models/status" | jq
curl -s "http://127.0.0.1:8080/admin/backends/status" | jq
```

## 9. 错误请求示例
```bash
curl -i -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3.5-2b","messages":"bad"}'
```

预期：`HTTP 400`。

## 10. 远程后端示例
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen3.5-2b",
    "inference_backend":"rpc",
    "messages":[{"role":"user","content":"hello from remote"}]
  }' | jq
```

备注：
- 远程 `stackflow` 返回里的 `usage` 当前为近似值。

## 11. 治理示例
超时：
```bash
HTTP_REQUEST_TIMEOUT_MS=1 ./build/serving/http/serving_http_server
curl -i -s -X POST "http://127.0.0.1:8080/v1/embeddings" \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3.5-2b","input":"hello embeddings"}'
```

预期：`HTTP 504`，错误码 `request_timeout`。

限流：
```bash
curl -s "http://127.0.0.1:8080/metrics" | jq
curl -s "http://127.0.0.1:8080/admin/backends/status" | jq
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
