#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
MODEL="${MODEL:-llama}"
TIMEOUT="${TIMEOUT:-40}"

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; exit 1; }

curl_json() {
  local method="$1"
  local url="$2"
  local data="${3:-}"
  if [[ -n "$data" ]]; then
    curl -sS --max-time "$TIMEOUT" -X "$method" "$url" -H "Content-Type: application/json" -d "$data"
  else
    curl -sS --max-time "$TIMEOUT" -X "$method" "$url"
  fi
}

echo "BASE_URL=$BASE_URL MODEL=$MODEL TIMEOUT=${TIMEOUT}s"

models_body="$(curl_json GET "$BASE_URL/v1/models")" || fail "GET /v1/models failed"
echo "$models_body" | rg -q '"object"\s*:\s*"list"' || fail "GET /v1/models response invalid"
pass "GET /v1/models"

health_body="$(curl_json GET "$BASE_URL/health")" || fail "GET /health failed"
echo "$health_body" | rg -q '"status"\s*:\s*"ok"' || fail "GET /health response invalid"
pass "GET /health"

metrics_body="$(curl_json GET "$BASE_URL/metrics")" || fail "GET /metrics failed"
echo "$metrics_body" | rg -q 'requests_total|avg_latency_ms' || fail "GET /metrics response invalid"
pass "GET /metrics"

non_stream_payload="{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}],\"max_tokens\":64}"
non_stream_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$non_stream_payload")" || fail "non-stream request failed"
echo "$non_stream_body" | rg -q '"choices"|"error"' || fail "non-stream response invalid"
pass "POST /v1/chat/completions (non-stream)"

analysis_payload="{\"model\":\"$MODEL\",\"agent\":true,\"agent_mode\":\"code_analysis\",\"max_steps\":4,\"tools\":[\"search_code\",\"read_file\",\"list_files\",\"search_docs\",\"get_config\",\"get_server_status\"],\"messages\":[{\"role\":\"user\",\"content\":\"HttpGateway 里 agent 请求是怎么进入 AgentExecutor 的？\"}],\"max_tokens\":128}"
analysis_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$analysis_payload")" || fail "analysis agent request failed"
echo "$analysis_body" | rg -q '"choices"|"error"' || fail "analysis agent response invalid"
pass "POST /v1/chat/completions (analysis agent)"

stream_payload="{\"model\":\"$MODEL\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"介绍一下你自己\"}],\"max_tokens\":64}"
stream_tmp="$(mktemp)"
trap 'rm -f "$stream_tmp"' EXIT
curl -sS -N --max-time "$TIMEOUT" -X POST "$BASE_URL/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d "$stream_payload" > "$stream_tmp" || fail "stream request failed"

rg -q '^data:' "$stream_tmp" || fail "stream response missing data chunks"
rg -q 'data: \[DONE\]' "$stream_tmp" || fail "stream response missing [DONE]"
pass "POST /v1/chat/completions (stream)"

stream_q_payload="{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"请简单打个招呼\"}],\"max_tokens\":32}"
stream_q_tmp="$(mktemp)"
trap 'rm -f "$stream_tmp" "$stream_q_tmp"' EXIT
curl -sS -N --max-time "$TIMEOUT" -X POST "$BASE_URL/v1/chat/completions?stream=true" \
  -H "Content-Type: application/json" \
  -d "$stream_q_payload" > "$stream_q_tmp" || fail "stream fallback query request failed"

rg -q '^data:' "$stream_q_tmp" || fail "stream fallback query response missing data chunks"
rg -q 'data: \[DONE\]' "$stream_q_tmp" || fail "stream fallback query response missing [DONE]"
pass "POST /v1/chat/completions?stream=true (fallback)"

bad_payload='{"model":"llama","messages":"bad"}'
status_code="$(curl -sS --max-time "$TIMEOUT" -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/v1/chat/completions" -H "Content-Type: application/json" -d "$bad_payload")"
[[ "$status_code" == "400" ]] || fail "invalid request expected HTTP 400, got $status_code"
pass "invalid payload returns HTTP 400"

echo "All smoke tests passed."
