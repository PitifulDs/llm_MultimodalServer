#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
LOG_DIR="/tmp/llm_serving"
WEB_PORT="${WEB_PORT:-8000}"
HTTP_PORT="${HTTP_PORT:-8080}"
DEMO_URL="http://127.0.0.1:${WEB_PORT}"

mkdir -p "${LOG_DIR}"
cd "${ROOT}"

tail_log() {
  log_file="$1"
  if [ -f "${log_file}" ]; then
    echo "last log lines: ${log_file}" >&2
    tail -n 40 "${log_file}" >&2 || true
  fi
}

wait_for_http_ready() {
  name="$1"
  pid="$2"
  log_file="$3"
  url="$4"
  retry="${5:-20}"
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

sh "${ROOT}/scripts/start_all.sh"

if [ -f "${LOG_DIR}/demo_web.pid" ]; then
  kill "$(cat "${LOG_DIR}/demo_web.pid")" 2>/dev/null || true
fi

nohup sh "${ROOT}/demo/web/serve_demo.sh" "${WEB_PORT}" > "${LOG_DIR}/demo_web.log" 2>&1 &
echo $! > "${LOG_DIR}/demo_web.pid"
wait_for_http_ready "demo_web" "$(cat "${LOG_DIR}/demo_web.pid")" "${LOG_DIR}/demo_web.log" "${DEMO_URL}/"

echo "started: demo_web     pid=$(cat "${LOG_DIR}/demo_web.pid")"
echo "demo url: ${DEMO_URL}"
echo "api url : http://127.0.0.1:${HTTP_PORT}"
echo "mainline endpoints:"
echo "  - POST /v1/chat/completions"
echo "  - GET  /v1/models"
echo "  - POST /v1/embeddings"
echo "  - POST /v1/rerank"
echo "  - GET  /healthz"
echo "  - GET  /admin/models/status"
echo "  - GET  /admin/backends/status"
echo "compatibility extensions: agent/rag stay available behind EXPERIMENTAL_AGENT_API_ENABLED=1 and EXPERIMENTAL_RAG_API_ENABLED=1"
echo "logs    : ${LOG_DIR}"

if command -v xdg-open >/dev/null 2>&1; then
  xdg-open "${DEMO_URL}" >/dev/null 2>&1 || true
elif command -v open >/dev/null 2>&1; then
  open "${DEMO_URL}" >/dev/null 2>&1 || true
fi
