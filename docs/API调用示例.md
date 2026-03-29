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
  -d '{"model":"qwen2.5-1.5b","messages":[{"role":"user","content":"hello"}]}' | jq
```

获取模型列表：
```bash
curl -s "http://127.0.0.1:8080/v1/models" | jq
```

## 4. 流式 SSE chat
```bash
curl -N -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen2.5-1.5b","stream":true,"messages":[{"role":"user","content":"介绍下华为"}],"max_tokens":128}'
```

## 5. 带采样参数
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen2.5-1.5b",
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
  -d '{"model":"qwen2.5-1.5b","messages":"bad"}'
```

预期：`HTTP 400`。

## 7. 只读 Analysis Agent 示例
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen2.5-1.5b",
    "agent": true,
    "agent_mode": "code_analysis",
    "max_steps": 4,
    "tools": ["search_code","read_file","list_files","search_docs","get_config","get_server_status"],
    "messages":[
      {"role":"user","content":"HttpGateway 里 agent 请求是怎么进入 AgentExecutor 的？"}
    ]
  }' | jq
```

如果要验证真实模型是否真的触发了只读工具调用，可以运行：
```bash
bash scripts/smoke_test_analysis_agent.sh
```

## 8. 远程模型示例（通过模型名路由到 stackflow）
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen2.5-1.5b-remote","messages":[{"role":"user","content":"hello from remote"}]}' | jq
```

备注：
- 远程 `stackflow` 返回里的 `usage` 当前为近似值
