#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-18083}"
MODEL="${MODEL:-qwen2.5-1.5b}"
TIMEOUT="${TIMEOUT:-120}"
LOG_DIR="${LOG_DIR:-/tmp/analysis_agent_smoke_real}"
SERVER_BIN="${SERVER_BIN:-$ROOT/build/serving/http/serving_http_server}"
CONFIG_PATH="${CONFIG_PATH:-$ROOT/config.json}"

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; exit 1; }

mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR/server.log" "$LOG_DIR/response.json" "$LOG_DIR/server.pid"

cleanup() {
  if [[ -f "$LOG_DIR/server.pid" ]]; then
    kill "$(cat "$LOG_DIR/server.pid")" >/dev/null 2>&1 || true
    wait "$(cat "$LOG_DIR/server.pid")" 2>/dev/null || true
  fi
}
trap cleanup EXIT

CONFIG_PATH="$CONFIG_PATH" "$SERVER_BIN" "$PORT" >"$LOG_DIR/server.log" 2>&1 &
echo $! > "$LOG_DIR/server.pid"
sleep 3

payload="$(cat <<JSON
{
  "model": "$MODEL",
  "agent": true,
  "agent_mode": "code_analysis",
  "max_steps": 4,
  "tools": ["search_code", "read_file", "list_files", "search_docs", "get_config", "get_server_status"],
  "messages": [
    {
      "role": "user",
      "content": "HttpGateway 里 agent 请求是怎么进入 AgentExecutor 的？请基于仓库代码回答，并指出相关文件。"
    }
  ],
  "max_tokens": 192
}
JSON
)"

curl -sS --max-time "$TIMEOUT" \
  -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d "$payload" > "$LOG_DIR/response.json" || fail "analysis agent request failed"

rg -q '"choices"' "$LOG_DIR/response.json" || fail "analysis agent response missing choices"
pass "analysis agent returned choices"

rg -q '"content"' "$LOG_DIR/response.json" || fail "analysis agent response missing content"
pass "analysis agent returned content"

rg -q '\[agent\].*tool=' "$LOG_DIR/server.log" || fail "analysis agent did not emit tool-call log"
pass "analysis agent emitted tool-call log"

echo "response file: $LOG_DIR/response.json"
echo "server log   : $LOG_DIR/server.log"
echo "Analysis agent real-model smoke test passed."
