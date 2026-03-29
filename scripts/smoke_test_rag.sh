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

docs_payload="{\"model\":\"$MODEL\",\"max_tokens\":32,\"messages\":[{\"role\":\"user\",\"content\":\"rag index\"}],\"rag\":{\"enabled\":true,\"kb\":\"docs\",\"top_k\":1,\"mode\":\"lexical\",\"return_references\":true}}"
docs_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$docs_payload")" || fail "docs rag request failed"
echo "$docs_body" | rg -q '"references"\s*:' || fail "docs rag response missing references"
echo "$docs_body" | rg -q '"kb"\s*:\s*"docs"' || fail "docs rag response missing docs reference"
pass "docs rag"

repo_payload="{\"model\":\"$MODEL\",\"max_tokens\":32,\"messages\":[{\"role\":\"user\",\"content\":\"build rag references\"}],\"rag\":{\"enabled\":true,\"kb\":\"repo_code\",\"top_k\":1,\"mode\":\"lexical\",\"return_references\":true}}"
repo_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$repo_payload")" || fail "repo_code rag request failed"
echo "$repo_body" | rg -q '"references"\s*:' || fail "repo_code rag response missing references"
echo "$repo_body" | rg -q '"kb"\s*:\s*"repo_code"' || fail "repo_code rag response missing repo_code reference"
pass "repo_code rag"

plain_payload="{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}"
plain_body="$(curl_json POST "$BASE_URL/v1/chat/completions" "$plain_payload")" || fail "plain chat request failed"
echo "$plain_body" | rg -q '"choices"|"error"' || fail "plain chat response invalid"
if echo "$plain_body" | rg -q '"references"\s*:'; then
  fail "plain chat unexpectedly contains references"
fi
pass "rag disabled path"

echo "RAG smoke tests passed."
