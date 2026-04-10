# RAG v2 Self-Test Record

Date: 2026-04-01
Branch: `feature/rag-support`
Scope: build, unit test, index build, retrieval API, stream metadata, agent tool path

## Summary

RAG v2 mainline is buildable and the retrieval path is runnable.

Verified pass:
- build targets: `serving_http_server`, `http_utils_test`, `rag_test`
- unit tests: `http_utils_test`, `rag_test`, `tests/unit/rag_chunkers_test.py`
- offline index scripts: `tools/rag/build_index.py --rebuild`, `tools/rag/build_vector_index.py --rebuild`
- service health: `GET /health`
- smoke items:
  - lexical chat RAG
  - hybrid chat RAG
  - retrieval debug API `/v1/retrieval/search`
  - agent `search_kb`
  - stream metadata / references before `[DONE]`

## Commands

Build:
```bash
cmake --build build --target serving_http_server http_utils_test rag_test -j4
```

Unit tests:
```bash
./build/tests/unit/http_utils_test
./build/tests/unit/rag_test
python3 tests/unit/rag_chunkers_test.py
```

Index rebuild:
```bash
python3 tools/rag/build_index.py --repo-root . --output data/rag_index.sqlite --rebuild
python3 tools/rag/build_vector_index.py \
  --repo-root . \
  --chunks-output data/rag_chunks.jsonl \
  --embeddings-output data/rag_embeddings.npy \
  --faiss-output data/rag_faiss.index \
  --id-map-output data/rag_id_map.json \
  --rebuild
```

Service:
```bash
WARMUP_MODEL=0 ./build/serving/http/serving_http_server
curl -sS http://127.0.0.1:8080/health
```

Smoke:
```bash
BASE_URL=http://127.0.0.1:8080 MODEL=llama TIMEOUT=180 bash scripts/smoke_test_rag_v2.sh
```

Standalone stream validation:
```bash
curl -sS --max-time 180 -N -X POST http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model":"llama",
    "max_tokens":64,
    "stream":true,
    "messages":[{"role":"user","content":"stream metadata references"}],
    "rag":{"enabled":true,"kb":"repo_code","top_k":2,"mode":"hybrid","return_references":true,"debug":true}
  }'
```

## Results

### Build and Unit Tests

Passed:
- `ninja: no work to do.`
- `http_utils_test passed`
- `rag_test passed`
- `rag_chunkers_test passed`

### Index Build

Passed after fixes.

Observed output:
```text
index_path=.../data/rag_index.sqlite
selected_kbs=docs,repo_code
total_files=1271
total_chunks=24042
docs_files=15
docs_chunks=774
repo_code_files=1256
repo_code_chunks=23268

chunks_path=.../data/rag_chunks.jsonl
embeddings_path=.../data/rag_embeddings.npy
faiss_index_path=.../data/rag_faiss.index
id_map_path=.../data/rag_id_map.json
```

### HTTP / Retrieval / Stream

Passed:
- `/health` returned `{"status":"ok",...}`
- lexical chat RAG passed in smoke
- hybrid chat RAG passed in smoke
- `/v1/retrieval/search` passed in smoke
- `scripts/smoke_test_rag_v2.sh` fully passed
- standalone stream request completed and returned:
  - normal delta chunks
  - a metadata chunk containing `metadata.references` and `metadata.retrieval`
  - final `[DONE]`

### Agent

Passed in smoke after optimization.

Current status:
- the previous hard failure `LlamaEngine: ctx/session null` is fixed
- a tool-only fast path is added for requests that only expose `search_kb` and `open_chunk`
- this removed the multi-step local-model planning overhead from the smoke case
- `scripts/smoke_test_rag_v2.sh` now passes the agent case reliably

## Issues Found During Self-Test

### 1. `build_index.py --rebuild` schema reuse bug

Symptom:
```text
sqlite3.OperationalError: table chunks has no column named prev_chunk_id
```

Cause:
- rebuild path reused an old SQLite file with the old v1 schema

Fix:
- on `--rebuild`, remove the existing sqlite file before recreating schema

### 2. Duplicate `chunk_id` during index build

Symptom:
```text
sqlite3.IntegrityError: UNIQUE constraint failed: chunks.chunk_id
```

Cause:
- upgraded chunker could emit duplicate chunks for some large headers

Fix:
- dedupe per-file chunks during index build and recompute `prev_chunk_id` / `next_chunk_id`

### 3. Agent sub-request missing session

Symptom:
```text
{"error":{"code":"internal_error","message":"LlamaEngine: ctx/session null","type":"internal_error"}}
```

Cause:
- `AgentExecutor` created step contexts with `session = nullptr`

Fix:
- inherit `ctx->session` into agent step contexts

### 4. Smoke script false positive and stream timeout

Problems:
- agent smoke accepted `"error"` as a pass condition
- stream smoke omitted `max_tokens`, causing long-running requests

Fixes:
- agent smoke now requires `choices` and rejects `error`
- stream smoke now sends `max_tokens=64`

### 5. Agent smoke latency

Symptom:
- tool-only agent requests were doing multiple model planning turns and could exceed the practical smoke budget

Fix:
- add a deterministic fast path in `AgentExecutor` when the allowed tool set is only `search_kb` and `open_chunk`
- the fast path runs retrieval directly, optionally opens the top chunk, and returns immediately without extra model planning turns

## Follow-up

Recommended next work:
- narrow the repo_code indexing scope or default retrieval scope to avoid noisy hits from `thirds/`
- consider a smaller/cheaper model profile for smoke and agent validation
