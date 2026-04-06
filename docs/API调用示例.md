# 接口说明与调用示例

默认服务地址：`http://127.0.0.1:8080`

Agent 对外口径：

- `code_analysis`：主推模式，优先仓库和本地证据
- `web_research`：补充模式，用于外部资料交叉验证
- `generic`：保留内部实验用途，不作为公开接口模式

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

## 8. Web Research 交叉验证示例
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen2.5-1.5b",
    "agent": true,
    "agent_mode": "web_research",
    "max_steps": 8,
    "tools": ["search_kb","open_chunk","search_code","read_file","list_files","search_docs","search_web","fetch_url","get_config","get_server_status"],
    "messages":[
      {"role":"user","content":"结合仓库和 http://example.com/ 页面，说明 web_research 的 references 是否会保留外部 URL。"}
    ]
  }' | jq
```

## 9. Code Analysis Agent 调试模式
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

## 10. Agent Debug 接口
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

`/v1/agent/debug` 对外只支持 `code_analysis` 和 `web_research` 两个公开模式；`generic` 不作为公开调试模式。

## 11. 验证脚本
如果要验证真实模型是否真的触发了只读工具调用，可以运行：
```bash
bash scripts/smoke_test_analysis_agent.sh
bash scripts/smoke_test_agent_code_analysis.sh
bash scripts/smoke_test_agent_web_research.sh
```

如果要跑小评测集：
```bash
python3 tools/agent/eval_code_analysis.py --base-url http://127.0.0.1:8080 --model llama
python3 tools/agent/eval_code_analysis.py --mode web_research --base-url http://127.0.0.1:8080 --model llama
```

## 12. 远程后端示例（同一逻辑模型切到 rpc）
```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3.5-2b","inference_backend":"rpc","messages":[{"role":"user","content":"hello from remote"}]}' | jq
```

备注：
- 远程 `stackflow` 返回里的 `usage` 当前为近似值
