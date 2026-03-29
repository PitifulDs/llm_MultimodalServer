#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
LOG_DIR="/tmp/llm_serving"

kill_pidfile() {
  pidfile="$1"
  if [ ! -f "$pidfile" ]; then
    return 0
  fi

  pid="$(cat "$pidfile" 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null || true
  fi
  rm -f "$pidfile"
}

kill_pattern() {
  pattern="$1"
  pkill -TERM -f "$pattern" 2>/dev/null || true
}

force_kill_pattern() {
  pattern="$1"
  pkill -KILL -f "$pattern" 2>/dev/null || true
}

kill_pidfile "${LOG_DIR}/serving_http.pid"
kill_pidfile "${LOG_DIR}/demo_web.pid"
kill_pidfile "${LOG_DIR}/node_test.pid"
kill_pidfile "${LOG_DIR}/unit_manager.pid"

kill_pattern "${ROOT}/build/serving/http/serving_http_server|\\./build/serving/http/serving_http_server"
kill_pattern "${ROOT}/unit-manager/build/unit_manager|\\./unit-manager/build/unit_manager"
kill_pattern "${ROOT}/node/test/build/test|\\./node/test/build/test"
kill_pattern "${ROOT}/demo/web/serve_demo.sh"
kill_pattern "python3 -m http.server .*${ROOT}/demo/web"

sleep 1

force_kill_pattern "${ROOT}/build/serving/http/serving_http_server|\\./build/serving/http/serving_http_server"
force_kill_pattern "${ROOT}/unit-manager/build/unit_manager|\\./unit-manager/build/unit_manager"
force_kill_pattern "${ROOT}/node/test/build/test|\\./node/test/build/test"
force_kill_pattern "${ROOT}/demo/web/serve_demo.sh"
force_kill_pattern "python3 -m http.server .*${ROOT}/demo/web"

echo "stopped (if running)."
