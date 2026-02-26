# API 调用示例

默认服务地址：`http://127.0.0.1:8080`

## 1. 健康检查
```bash
curl -s http://127.0.0.1:8080/health | jq
```

## 2. 指标
```bash
curl -s http://127.0.0.1:8080/metrics | jq
```

## 3. 非流式 chat
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"llama","messages":[{"role":"user","content":"hello"}]}' | jq
```

## 4. 流式 SSE chat
```bash
curl -N -X POST "http://127.0.0.1:8080/v1/chat/completions?stream=true" \
  -H "Content-Type: application/json" \
  -d '{"model":"llama","messages":[{"role":"user","content":"介绍下华为"}],"max_tokens":128}'
```

## 5. 带采样参数
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"llama",
    "messages":[{"role":"user","content":"写一个快速排序"}],
    "max_tokens":200,
    "temperature":0.7,
    "top_p":0.9,
    "top_k":40,
    "repeat_penalty":1.1
  }' | jq
```

## 6. 错误请求示例（messages 非数组）
```bash
curl -i -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"llama","messages":"bad"}'
```

预期：`HTTP 400`。
