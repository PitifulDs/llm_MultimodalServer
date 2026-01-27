# llm_MultimodalServer

A lightweight LLM serving stack with:
- HTTP OpenAI-compatible endpoint
- Local llama.cpp engine
- StackFlow remote engine (unit-manager + worker)
- Simple demo web UI

## Quick Start (Local llama.cpp)

Build:

```bash
cmake -S . -B build
cmake --build build -j
```

Run:

```bash
./build/serving/http/serving_http_server
```

By default it reads `config.json` in repo root.

Test:

```bash
curl -s -X POST "http://127.0.0.1:8080/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{"model":"llama","messages":[{"role":"user","content":"hello"}]}' | jq
```

## StackFlow Remote Mode (unit-manager + worker)

Start in order:

1) unit-manager
```bash
./unit-manager/build/unit_manager
```

2) worker (`node/test`)
```bash
./node/test/build/test
```

3) HTTP server
```bash
./build/serving/http/serving_http_server
```

Set `serving_backend` to `stackflow` in `config.json` or via env `SERVING_BACKEND=stackflow`.

### Worker model path

`node/test` uses a default model path in code. You can override via env:

```bash
export STACKFLOW_MODEL_PATH=/path/to/model.gguf
```

If you change the model path, rebuild `node/test`:

```bash
rm -rf node/test/build
cmake -S node/test -B node/test/build
cmake --build node/test/build -j
```

## Config

`config.json` keys (partial):
- `http_port`
- `default_model`
- `llama_model_path`
- `llama_n_ctx`, `llama_n_threads`, `llama_n_threads_batch`
- `default_max_tokens`
- `serving_backend` (`local` or `stackflow`)
- `stackflow_host`, `stackflow_port`, `stackflow_unit`, `stackflow_timeout_ms`

You can also set `CONFIG_PATH` to load a different config file.

## Demo Web UI

Serve the demo UI:

```bash
bash demo/web/serve_demo.sh 8000
```

Open in browser:
- `http://<your-host-ip>:8000/`

The UI supports stream/non-stream toggle and shows request duration.

## Troubleshooting

- If `node/test` does not exit on Ctrl+C, ensure you run it in the foreground.
- If StackFlow setup times out, clear IPC sockets:

```bash
rm -f /tmp/llm/*.sock*
```

Then restart `unit-manager` and `node/test`.
