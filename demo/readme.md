# LLM Serving v1 (C++ / llama.cpp / TCP / JSON)
### 架构流程图
    TCP client
        |
    JSON line protocol
        |
    handle_client()
        |
    parse JSON
        |
    get_or_create_session(session_id)
        |
    LLMUnit::Generate / GenerateStream
        |   
    llama.cpp

### LLM Serving v1 架构
    支持 session（KV cache 持久）
    支持 reset / exit
    支持 stream
    支持 JSON 协议（可被 Web / Python 调）

### Session状态
    | 行为           | 规则                       |
    | ------------   | -----------------------   |
    | 新 session_id | 新建 LLMUnit                |
    | 同 session_id | 复用 LLMUnit + history      |
    | reset         | 清空 history + 重建 LLMUnit |
    | exit          | 关闭 TCP 连接               |


### demo使用
1. 启动服务# 启动服务
   
    ./llm_server --model qwen2.5-1.5b-instruct-q4_0.gguf

2. 客户端
    
    nc 127.0.0.1 9000

## Json 协议
```json
请求（非流式）
{
  "type": "chat",
  "session_id": "alice",
  "prompt": "介绍一下 C++",
  "stream": false
}

请求（流式）
{
  "type": "chat",
  "session_id": "alice",
  "prompt": "介绍一下 C++",
  "stream": true
}

响应（非流式）
{
  "type": "response",
  "session_id": "alice",
  "reply": "...",
  "finish_reason": "stop"
}
响应（流式）
{"type":"start","session_id":"alice"}
{"type":"chunk","session_id":"alice","delta":"C++"}
{"type":"chunk","session_id":"alice","delta":" 是一种"}
{"type":"end","session_id":"alice","finish_reason":"stop"}
```
### Notes
服务端维护每个 session 的对话历史，并在超过上下文限制时自动裁剪历史或重建模型上下文。
- 基于 llama.cpp v0.0.7402
- 不显式控制 KV cache
- 长对话通过 history 裁剪 / 重建 context 方式处理
- 后续版本可升级至 llama.cpp 新 API 以支持 KV cache 管理