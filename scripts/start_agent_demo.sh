#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="/tmp/llm_serving"
WEB_PORT="${WEB_PORT:-8000}"
HTTP_PORT="${HTTP_PORT:-8080}"
DEMO_URL="http://127.0.0.1:${WEB_PORT}"

mkdir -p "${LOG_DIR}"
cd "${ROOT}"

bash "${ROOT}/scripts/start_all.sh"

if [ -f "${LOG_DIR}/demo_web.pid" ]; then
  kill "$(cat "${LOG_DIR}/demo_web.pid")" 2>/dev/null || true
fi

nohup bash "${ROOT}/demo/web/serve_demo.sh" "${WEB_PORT}" > "${LOG_DIR}/demo_web.log" 2>&1 &
echo $! > "${LOG_DIR}/demo_web.pid"

echo "started: demo_web     pid=$(cat "${LOG_DIR}/demo_web.pid")"
echo "demo url: ${DEMO_URL}"
echo "api url : http://127.0.0.1:${HTTP_PORT}"
echo "logs    : ${LOG_DIR}"

if command -v xdg-open >/dev/null 2>&1; then
  xdg-open "${DEMO_URL}" >/dev/null 2>&1 || true
elif command -v open >/dev/null 2>&1; then
  open "${DEMO_URL}" >/dev/null 2>&1 || true
fi
