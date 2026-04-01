#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-18084}"
MODEL="${MODEL:-llama}"
TIMEOUT="${TIMEOUT:-120}"
LOG_DIR="${LOG_DIR:-/tmp/agent_code_analysis_smoke}"
SERVER_BIN="${SERVER_BIN:-$ROOT/build/serving/http/serving_http_server}"
CONFIG_PATH="${CONFIG_PATH:-$ROOT/config.json}"

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; exit 1; }

mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/server.log "$LOG_DIR"/*.json "$LOG_DIR"/server.pid

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

kb_payload="$(cat <<JSON
{
  "model": "$MODEL",
  "agent": true,
  "agent_mode": "code_analysis",
  "agent_debug": true,
  "agent_output_format": "structured",
  "tools": ["search_kb", "open_chunk"],
  "messages": [
    {
      "role": "user",
      "content": "stream metadata references 在哪里输出"
    }
  ]
}
JSON
)"

curl -sS --max-time "$TIMEOUT" \
  -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d "$kb_payload" > "$LOG_DIR/kb_open_chunk.json" || fail "search_kb/open_chunk request failed"

rg -q '"agent_trace"' "$LOG_DIR/kb_open_chunk.json" || fail "structured response missing agent_trace"
rg -q '"selected_tool":"search_kb"' "$LOG_DIR/kb_open_chunk.json" || fail "trace missing search_kb"
rg -q '"selected_tool":"open_chunk"' "$LOG_DIR/kb_open_chunk.json" || fail "trace missing open_chunk"
rg -q '"evidence"' "$LOG_DIR/kb_open_chunk.json" || fail "structured response missing evidence"
pass "search_kb -> open_chunk path"

code_payload="$(cat <<JSON
{
  "model": "$MODEL",
  "agent": true,
  "agent_mode": "code_analysis",
  "agent_debug": true,
  "tools": ["search_code", "read_file"],
  "messages": [
    {
      "role": "user",
      "content": "HttpGateway::HandleChatCompletion 里 agent_executor_->Run 是怎么被调用的"
    }
  ]
}
JSON
)"

curl -sS --max-time "$TIMEOUT" \
  -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d "$code_payload" > "$LOG_DIR/search_code_read_file.json" || fail "search_code/read_file request failed"

rg -q '"selected_tool":"search_code"' "$LOG_DIR/search_code_read_file.json" || fail "trace missing search_code"
rg -q '"selected_tool":"read_file"' "$LOG_DIR/search_code_read_file.json" || fail "trace missing read_file"
rg -q 'serving/http/HttpGateway.cc' "$LOG_DIR/search_code_read_file.json" || fail "response missing expected evidence path"
pass "search_code -> read_file path"

debug_payload="$(cat <<JSON
{
  "model": "$MODEL",
  "mode": "code_analysis",
  "debug": true,
  "query": "references 是在哪里拼出来的",
  "tools": ["search_code", "read_file", "search_kb", "open_chunk"]
}
JSON
)"

curl -sS --max-time "$TIMEOUT" \
  -X POST "http://127.0.0.1:${PORT}/v1/agent/debug" \
  -H "Content-Type: application/json" \
  -d "$debug_payload" > "$LOG_DIR/agent_debug.json" || fail "/v1/agent/debug request failed"

rg -q '"planner_steps"' "$LOG_DIR/agent_debug.json" || fail "debug response missing planner_steps"
rg -q '"final_answer"' "$LOG_DIR/agent_debug.json" || fail "debug response missing final_answer"
pass "debug trace endpoint"

echo "kb/open_chunk response : $LOG_DIR/kb_open_chunk.json"
echo "code/read_file response: $LOG_DIR/search_code_read_file.json"
echo "debug response         : $LOG_DIR/agent_debug.json"
echo "server log             : $LOG_DIR/server.log"
