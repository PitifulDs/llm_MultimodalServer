#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
LOG_DIR="/tmp/llm_serving"
CFG_PATH="${CONFIG_PATH:-${ROOT}/config.json}"
HTTP_PORT="${HTTP_PORT:-8080}"
export CFG_PATH

mkdir -p "${LOG_DIR}"
cd "${ROOT}"

tail_log() {
  log_file="$1"
  if [ -f "${log_file}" ]; then
    echo "last log lines: ${log_file}" >&2
    tail -n 40 "${log_file}" >&2 || true
  fi
}

wait_for_pid_alive() {
  name="$1"
  pid="$2"
  log_file="$3"
  retry="${4:-20}"
  i=0
  while [ "${i}" -lt "${retry}" ]; do
    if kill -0 "${pid}" 2>/dev/null; then
      sleep 0.2
      if kill -0 "${pid}" 2>/dev/null; then
        return 0
      fi
    else
      break
    fi
    i=$((i + 1))
  done
  echo "failed: ${name} exited right after start (pid=${pid})" >&2
  tail_log "${log_file}"
  exit 1
}

wait_for_http_ready() {
  name="$1"
  pid="$2"
  log_file="$3"
  url="$4"
  retry="${5:-40}"
  i=0
  while [ "${i}" -lt "${retry}" ]; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      echo "failed: ${name} exited before ready check passed (pid=${pid})" >&2
      tail_log "${log_file}"
      exit 1
    fi
    if curl -fsS --max-time 2 "${url}" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
    i=$((i + 1))
  done
  echo "failed: ${name} did not become ready: ${url}" >&2
  tail_log "${log_file}"
  exit 1
}

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
emit("llama_model_path", "LLAMA_MODEL_PATH")
emit("llama_model_path", "LLM_MODEL_PATH")
emit("stackflow_max_concurrency", "STACKFLOW_MAX_CONCURRENCY")
PY
  )"
fi

# resolve relative model path from config to absolute path for worker
if [ -n "${STACKFLOW_MODEL_PATH:-}" ]; then
  case "${STACKFLOW_MODEL_PATH}" in
    /*) ;;
    *) export STACKFLOW_MODEL_PATH="${ROOT}/${STACKFLOW_MODEL_PATH}" ;;
  esac
fi
if [ -n "${LLAMA_MODEL_PATH:-}" ]; then
  case "${LLAMA_MODEL_PATH}" in
    /*) ;;
    *) export LLAMA_MODEL_PATH="${ROOT}/${LLAMA_MODEL_PATH}" ;;
  esac
fi
if [ -n "${LLM_MODEL_PATH:-}" ]; then
  case "${LLM_MODEL_PATH}" in
    /*) ;;
    *) export LLM_MODEL_PATH="${ROOT}/${LLM_MODEL_PATH}" ;;
  esac
fi

# runtime shared libraries
export LD_LIBRARY_PATH="${ROOT}/build/bin:${ROOT}/build/network:${ROOT}/node/test/build/bin:${LD_LIBRARY_PATH:-}"

# stop any previous processes, including stale residual ones
sh "${ROOT}/scripts/stop_all.sh"

# clear old IPC sockets
rm -f /tmp/llm/*.sock* /tmp/rpc.* || true

nohup "${ROOT}/unit-manager/build/unit_manager" > "${LOG_DIR}/unit_manager.log" 2>&1 &
echo $! > "${LOG_DIR}/unit_manager.pid"
wait_for_pid_alive "unit_manager" "$(cat "${LOG_DIR}/unit_manager.pid")" "${LOG_DIR}/unit_manager.log"

nohup "${ROOT}/node/test/build/test" > "${LOG_DIR}/node_test.log" 2>&1 &
echo $! > "${LOG_DIR}/node_test.pid"
wait_for_pid_alive "node_test" "$(cat "${LOG_DIR}/node_test.pid")" "${LOG_DIR}/node_test.log"

nohup "${ROOT}/build/serving/http/serving_http_server" "${HTTP_PORT}" > "${LOG_DIR}/serving_http.log" 2>&1 &
echo $! > "${LOG_DIR}/serving_http.pid"
wait_for_http_ready "serving_http" "$(cat "${LOG_DIR}/serving_http.pid")" "${LOG_DIR}/serving_http.log" "http://127.0.0.1:${HTTP_PORT}/health"

echo "started: unit_manager pid=$(cat "${LOG_DIR}/unit_manager.pid")"
echo "started: node_test   pid=$(cat "${LOG_DIR}/node_test.pid")"
echo "started: serving_http pid=$(cat "${LOG_DIR}/serving_http.pid")"
echo "logs: ${LOG_DIR}"
