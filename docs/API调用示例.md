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

## 7. Code Analysis Agent 基础示例
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen2.5-1.5b",
    "agent": true,
    "agent_mode": "code_analysis",
    "max_steps": 4,
    "tools": ["search_kb","open_chunk","search_code","read_file","list_files","search_docs","get_config","get_server_status"],
    "messages":[
      {"role":"user","content":"HttpGateway 里 agent 请求是怎么进入 AgentExecutor 的？"}
    ]
  }' | jq
```

## 8. Code Analysis Agent 调试模式
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen2.5-1.5b",
    "agent": true,
    "agent_mode": "code_analysis",
    "agent_debug": true,
    "agent_include_trace": true,
    "agent_output_format": "structured",
    "messages":[
      {"role":"user","content":"references 是在哪里拼出来的？"}
    ]
  }' | jq
```

预期非流式返回里会带：
- `agent_result`
- `evidence`
- `agent_trace`

## 9. Agent Debug 接口
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/agent/debug" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen2.5-1.5b",
    "mode":"code_analysis",
    "debug":true,
    "query":"stream metadata 在哪一层输出"
  }' | jq
```

返回里主要关注：
- `planner_steps`
- `evidence`
- `final_answer`

## 10. 验证脚本
如果要验证真实模型是否真的触发了只读工具调用，可以运行：
```bash
bash scripts/smoke_test_analysis_agent.sh
bash scripts/smoke_test_agent_code_analysis.sh
```

如果要跑小评测集：
```bash
python3 tools/agent/eval_code_analysis.py --base-url http://127.0.0.1:8080 --model llama
```

## 11. 远程后端示例（同一逻辑模型切到 rpc）
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3.5-2b","inference_backend":"rpc","messages":[{"role":"user","content":"hello from remote"}]}' | jq
```

备注：
- 远程 `stackflow` 返回里的 `usage` 当前为近似值
