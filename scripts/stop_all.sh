#!/bin/sh
set -eu

LOG_DIR="/tmp/llm_serving"

if [ -f "${LOG_DIR}/serving_http.pid" ]; then
  kill "$(cat "${LOG_DIR}/serving_http.pid")" 2>/dev/null || true
fi
if [ -f "${LOG_DIR}/demo_web.pid" ]; then
  kill "$(cat "${LOG_DIR}/demo_web.pid")" 2>/dev/null || true
fi
if [ -f "${LOG_DIR}/node_test.pid" ]; then
  kill "$(cat "${LOG_DIR}/node_test.pid")" 2>/dev/null || true
fi
if [ -f "${LOG_DIR}/unit_manager.pid" ]; then
  kill "$(cat "${LOG_DIR}/unit_manager.pid")" 2>/dev/null || true
fi

echo "stopped (if running)."
