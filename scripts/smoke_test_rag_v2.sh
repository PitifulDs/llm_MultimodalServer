#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
MODEL="${MODEL:-llama}"
TIMEOUT="${TIMEOUT:-90}"

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; exit 1; }

curl_json() {
  local method="$1"
  local url="$2"
  local data="${3:-}"
  curl -sS --max-time "$TIMEOUT" -X "$method" "$url" -H "Content-Type: application/json" -d "$data"
}

echo "BASE_URL=$BASE_URL MODEL=$MODEL TIMEOUT=${TIMEOUT}s"

lexical_payload="{\"model\":\"$MODEL\",\"max_tokens\":32,\"messages\":[{\"role\":\"user\",\"content\":\"rag index\"}],\"rag\":{\"enabled\":true,\"kb\":\"docs\",\"top_k\":1,\"mode\":\"lexical\",\"return_references\":true}}"
lexical_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$lexical_payload")" || fail "lexical rag request failed"
echo "$lexical_body" | rg -q '"references"\s*:' || fail "lexical rag response missing references"
pass "lexical"

hybrid_payload="{\"model\":\"$MODEL\",\"max_tokens\":48,\"messages\":[{\"role\":\"user\",\"content\":\"stream metadata references\"}],\"rag\":{\"enabled\":true,\"kb\":\"repo_code\",\"top_k\":2,\"mode\":\"hybrid\",\"lexical_top_k\":4,\"vector_top_k\":4,\"fusion\":\"rrf\",\"return_references\":true,\"debug\":true}}"
hybrid_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$hybrid_payload")" || fail "hybrid rag request failed"
echo "$hybrid_body" | rg -q '"retrieval"\s*:' || fail "hybrid rag response missing retrieval debug block"
pass "hybrid"

search_payload='{"kb":"repo_code","query":"stream metadata references","mode":"hybrid","top_k":2,"debug":true}'
search_body="$(curl_json POST "$BASE_URL/v1/retrieval/search" "$search_payload")" || fail "retrieval debug api failed"
echo "$search_body" | rg -q '"normalized_query"\s*:' || fail "retrieval debug api missing normalized_query"
echo "$search_body" | rg -q '"hits"\s*:' || fail "retrieval debug api missing hits"
pass "retrieval debug api"

agent_payload="{\"model\":\"$MODEL\",\"max_tokens\":96,\"agent\":true,\"tools\":[\"search_kb\",\"open_chunk\"],\"messages\":[{\"role\":\"user\",\"content\":\"先检索知识库，说明 stream metadata references 在哪里。\"}]}"
agent_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$agent_payload")" || fail "agent search_kb request failed"
echo "$agent_body" | rg -q '"choices"\s*:' || fail "agent response missing choices"
if echo "$agent_body" | rg -q '"error"\s*:'; then
  fail "agent response returned error"
fi
pass "agent search_kb"

stream_payload="{\"model\":\"$MODEL\",\"max_tokens\":64,\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"stream metadata references\"}],\"rag\":{\"enabled\":true,\"kb\":\"repo_code\",\"top_k\":2,\"mode\":\"hybrid\",\"return_references\":true,\"debug\":true}}"
stream_body="$(curl -sS --max-time "$TIMEOUT" -N -X POST "$BASE_URL/v1/chat/completions" -H "Content-Type: application/json" -d "$stream_payload")" || fail "stream rag request failed"
echo "$stream_body" | rg -q '"metadata"\s*:' || fail "stream rag response missing metadata chunk"
echo "$stream_body" | rg -q '\[DONE\]' || fail "stream rag response missing DONE"
pass "stream references"

echo "RAG v2 smoke tests passed."
