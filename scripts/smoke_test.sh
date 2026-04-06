#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
MODEL="${MODEL:-qwen3.5-2b}"
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

health_body="$(curl_json GET "$BASE_URL/healthz")" || fail "GET /healthz failed"
echo "$health_body" | rg -q '"status"\s*:\s*"ok"' || fail "GET /healthz response invalid"
pass "GET /healthz"

admin_models_body="$(curl_json GET "$BASE_URL/admin/models/status")" || fail "GET /admin/models/status failed"
echo "$admin_models_body" | rg -q '"object"\s*:\s*"list"' || fail "GET /admin/models/status response invalid"
pass "GET /admin/models/status"

admin_backends_body="$(curl_json GET "$BASE_URL/admin/backends/status")" || fail "GET /admin/backends/status failed"
echo "$admin_backends_body" | rg -q '"object"\s*:\s*"list"' || fail "GET /admin/backends/status response invalid"
pass "GET /admin/backends/status"

non_stream_payload="{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}],\"max_tokens\":64}"
non_stream_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$non_stream_payload")" || fail "non-stream request failed"
echo "$non_stream_body" | rg -q '"choices"|"error"' || fail "non-stream response invalid"
pass "POST /v1/chat/completions (non-stream)"

embeddings_payload="{\"model\":\"$MODEL\",\"input\":\"hello embeddings\"}"
embeddings_body="$(curl_json POST "$BASE_URL/v1/embeddings" "$embeddings_payload")" || fail "embeddings request failed"
echo "$embeddings_body" | rg -q '"object"\s*:\s*"list"' || fail "embeddings response invalid"
echo "$embeddings_body" | rg -q '"embedding"\s*:' || fail "embeddings data missing"
pass "POST /v1/embeddings"

rerank_payload="{\"model\":\"$MODEL\",\"query\":\"hello rerank\",\"documents\":[\"totally unrelated weather report\",\"hello rerank\"],\"top_n\":1}"
rerank_body="$(curl_json POST "$BASE_URL/v1/rerank" "$rerank_payload")" || fail "rerank request failed"
echo "$rerank_body" | rg -q '"object"\s*:\s*"list"' || fail "rerank response invalid"
echo "$rerank_body" | rg -q '"relevance_score"\s*:' || fail "rerank data missing"
pass "POST /v1/rerank"

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
