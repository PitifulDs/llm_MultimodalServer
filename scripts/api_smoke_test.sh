#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
MODEL="${MODEL:-qwen3.5-2b}"
TIMEOUT="${TIMEOUT:-120}"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1" >&2; exit 1; }

request_json() {
  local method="$1"
  local path="$2"
  local data="${3:-}"
  local output="$4"
  local status="$5"

  if [[ -n "$data" ]]; then
    curl -sS --max-time "$TIMEOUT" \
      -w '%{http_code}' \
      -o "$output" \
      -X "$method" "$BASE_URL$path" \
      -H 'Content-Type: application/json' \
      -d "$data" > "$status"
  else
    curl -sS --max-time "$TIMEOUT" \
      -w '%{http_code}' \
      -o "$output" \
      -X "$method" "$BASE_URL$path" > "$status"
  fi
}

expect_status() {
  local name="$1"
  local status_file="$2"
  local expected="$3"
  local actual
  actual="$(cat "$status_file")"
  [[ "$actual" == "$expected" ]] || fail "$name expected HTTP $expected, got $actual"
}

echo "BASE_URL=$BASE_URL"
echo "MODEL=$MODEL"

request_json GET "/v1/models" "" "$tmpdir/models.json" "$tmpdir/models.status"
expect_status "GET /v1/models" "$tmpdir/models.status" 200
python3 - "$tmpdir/models.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body.get("object") == "list"
print("models:", ", ".join(m.get("id", "?") for m in body.get("data", [])))
PY
pass "GET /v1/models"

chat_payload="$(python3 - "$MODEL" <<'PY'
import json, sys
print(json.dumps({
    "model": sys.argv[1],
    "inference_backend": "local",
    "messages": [{"role": "user", "content": "Reply with one short sentence."}],
    "max_tokens": 8,
}))
PY
)"
request_json POST "/v1/chat/completions" "$chat_payload" "$tmpdir/chat_local.json" "$tmpdir/chat_local.status"
expect_status "POST /v1/chat/completions local" "$tmpdir/chat_local.status" 200
python3 - "$tmpdir/chat_local.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
choice = body["choices"][0]
print("chat:", choice["finish_reason"], repr(choice["message"]["content"][:80]))
PY
pass "POST /v1/chat/completions local"

rpc_payload="$(python3 - "$MODEL" <<'PY'
import json, sys
print(json.dumps({
    "model": sys.argv[1],
    "inference_backend": "rpc",
    "messages": [{"role": "user", "content": "hello"}],
    "max_tokens": 4,
}))
PY
)"
request_json POST "/v1/chat/completions" "$rpc_payload" "$tmpdir/chat_rpc.json" "$tmpdir/chat_rpc.status"
expect_status "POST /v1/chat/completions rpc failure" "$tmpdir/chat_rpc.status" 400
python3 - "$tmpdir/chat_rpc.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
err = body.get("error", {})
assert err.get("code") in {"backend_not_available", "capability_not_supported"}
print("chat rpc failure:", err.get("code"), "-", err.get("message"))
PY
pass "POST /v1/chat/completions rpc failure"

embeddings_payload="$(python3 - "$MODEL" <<'PY'
import json, sys
print(json.dumps({"model": sys.argv[1], "input": "hello embeddings"}))
PY
)"
request_json POST "/v1/embeddings" "$embeddings_payload" "$tmpdir/embeddings.json" "$tmpdir/embeddings.status"
expect_status "POST /v1/embeddings" "$tmpdir/embeddings.status" 200
python3 - "$tmpdir/embeddings.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
vec = body["data"][0]["embedding"]
print("embeddings:", len(body["data"]), "vector(s), dim", len(vec))
PY
pass "POST /v1/embeddings"

rerank_payload="$(python3 - "$MODEL" <<'PY'
import json, sys
print(json.dumps({
    "model": sys.argv[1],
    "query": "hello",
    "documents": ["other", "hello world"],
    "top_n": 1,
}))
PY
)"
request_json POST "/v1/rerank" "$rerank_payload" "$tmpdir/rerank.json" "$tmpdir/rerank.status"
expect_status "POST /v1/rerank" "$tmpdir/rerank.status" 200
python3 - "$tmpdir/rerank.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
item = body["data"][0]
print("rerank:", "index", item["index"], "score", item["relevance_score"])
PY
pass "POST /v1/rerank"

echo "API smoke test passed."
