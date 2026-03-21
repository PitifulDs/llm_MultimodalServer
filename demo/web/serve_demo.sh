#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-8000}"

python3 -m http.server "$PORT" -d "$ROOT_DIR"
