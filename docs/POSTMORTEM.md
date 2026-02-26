# 故障复盘（Postmortem）

## Case 1: 第二次请求经常 timeout / 500

现象：
- 首次请求成功，后续请求超时或 `setup timeout`。

根因：
- 远程链路存在状态残留（work_id 复用冲突、旧连接/旧 socket 未清理）。

修复：
- 增加复用串行保护。
- 启停脚本统一清理 socket 与残留进程。
- 超时与错误码统一返回。

预防：
- 每次回归前执行 `scripts/stop_all.sh` + `scripts/start_all.sh`。
- 线上配置合理的 `stackflow_timeout_ms`。

## Case 2: JSON 抛出 UTF-8 type_error

现象：
- 后端日志出现 `incomplete UTF-8 string`。

根因：
- 流式分块可能截断多字节 UTF-8 字符，直接序列化触发异常。

修复：
- 在 `OpenAIStreamWriter` 中增加 UTF-8 pending 字节缓存与 flush 逻辑。

预防：
- 所有输出链路都做 UTF-8 安全处理，不假设 chunk 总是完整字符边界。

## Case 3: 流式请求卡住不结束

现象：
- 前端一直 `STREAMING`，看不到结束状态。

根因：
- 缺少 SSE 标准结束帧 `data: [DONE]` 或 finish 回调未触发。

修复：
- 在 writer 结束分支固定输出 `[DONE]`。
- 在 executor 加兜底 finish，防止引擎漏发结束事件。

预防：
- 把 `stream` 场景加入 smoke test，校验必须包含 `[DONE]`。
