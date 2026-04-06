#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
echo "warning: scripts/start_agent_demo.sh is deprecated; use scripts/start_platform_demo.sh" >&2
exec sh "${ROOT}/scripts/start_platform_demo.sh"
