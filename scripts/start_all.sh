#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="/tmp/llm_serving"
CFG_PATH="${CONFIG_PATH:-${ROOT}/config.json}"
export CFG_PATH

mkdir -p "${LOG_DIR}"
cd "${ROOT}"

# load config for worker env (model path / concurrency)
if [ -f "${CFG_PATH}" ]; then
  eval "$(
    python3 - <<'PY'
import json, os
cfg_path = os.environ.get("CFG_PATH")
with open(cfg_path, "r", encoding="utf-8") as f:
    cfg = json.load(f)
def emit(key, env):
    v = cfg.get(key)
    if v is None:
        return
    print(f'export {env}="{v}"')
emit("llama_model_path", "STACKFLOW_MODEL_PATH")
emit("stackflow_max_concurrency", "STACKFLOW_MAX_CONCURRENCY")
PY
  )"
fi

# resolve relative model path from config to absolute path for worker
if [ -n "${STACKFLOW_MODEL_PATH:-}" ] && [[ "${STACKFLOW_MODEL_PATH}" != /* ]]; then
  export STACKFLOW_MODEL_PATH="${ROOT}/${STACKFLOW_MODEL_PATH}"
fi

# stop any previous processes if pid files exist
if [ -f "${LOG_DIR}/serving_http.pid" ]; then
  kill "$(cat "${LOG_DIR}/serving_http.pid")" 2>/dev/null || true
fi
if [ -f "${LOG_DIR}/node_test.pid" ]; then
  kill "$(cat "${LOG_DIR}/node_test.pid")" 2>/dev/null || true
fi
if [ -f "${LOG_DIR}/unit_manager.pid" ]; then
  kill "$(cat "${LOG_DIR}/unit_manager.pid")" 2>/dev/null || true
fi

# clear old IPC sockets
rm -f /tmp/llm/*.sock* /tmp/rpc.* || true

nohup "${ROOT}/unit-manager/build/unit_manager" > "${LOG_DIR}/unit_manager.log" 2>&1 &
echo $! > "${LOG_DIR}/unit_manager.pid"

nohup "${ROOT}/node/test/build/test" > "${LOG_DIR}/node_test.log" 2>&1 &
echo $! > "${LOG_DIR}/node_test.pid"

nohup "${ROOT}/build/serving/http/serving_http_server" 8080 > "${LOG_DIR}/serving_http.log" 2>&1 &
echo $! > "${LOG_DIR}/serving_http.pid"

echo "started: unit_manager pid=$(cat "${LOG_DIR}/unit_manager.pid")"
echo "started: node_test   pid=$(cat "${LOG_DIR}/node_test.pid")"
echo "started: serving_http pid=$(cat "${LOG_DIR}/serving_http.pid")"
echo "logs: ${LOG_DIR}"
