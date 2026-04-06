#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-18085}"
MODEL="${MODEL:-llama}"
TIMEOUT="${TIMEOUT:-240}"
WEB_QUERY="${WEB_QUERY:-请基于 http://example.com/ 页面说明 Example Domain 是什么。}"
LOG_DIR="${LOG_DIR:-/tmp/agent_web_research_smoke}"
SERVER_BIN="${SERVER_BIN:-$ROOT/build/serving/http/serving_http_server}"
CONFIG_PATH="${CONFIG_PATH:-$ROOT/config.json}"

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; exit 1; }

wait_for_server() {
  local deadline=$((SECONDS + TIMEOUT))
  while (( SECONDS < deadline )); do
    if curl -fsS --max-time 2 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

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
wait_for_server || fail "server did not become healthy in time"

payload="$(cat <<JSON
{
  "model": "$MODEL",
  "mode": "web_research",
  "debug": true,
  "agent_output_format": "structured",
  "max_steps": 4,
  "tools": ["search_web", "fetch_url"],
  "query": "$WEB_QUERY"
}
JSON
)"

curl -sS --max-time "$TIMEOUT" \
  -X POST "http://127.0.0.1:${PORT}/v1/agent/debug" \
  -H "Content-Type: application/json" \
  -d "$payload" > "$LOG_DIR/web_research_debug.json" || fail "web_research debug request failed"

rg -q '"mode":"web_research"' "$LOG_DIR/web_research_debug.json" || fail "debug response missing web_research mode"
rg -q '"planner_steps"' "$LOG_DIR/web_research_debug.json" || fail "debug response missing planner_steps"
rg -q '"selected_tool":"search_web"' "$LOG_DIR/web_research_debug.json" || fail "trace missing search_web"
rg -q '"selected_tool":"fetch_url"' "$LOG_DIR/web_research_debug.json" || fail "trace missing fetch_url"
rg -q '"final_answer"' "$LOG_DIR/web_research_debug.json" || fail "debug response missing final_answer"
rg -q '"references"' "$LOG_DIR/web_research_debug.json" || fail "debug response missing references"
rg -q '"source":"web"' "$LOG_DIR/web_research_debug.json" || fail "references missing web source"
pass "search_web -> fetch_url -> synthesis path"

echo "response file: $LOG_DIR/web_research_debug.json"
echo "server log   : $LOG_DIR/server.log"
