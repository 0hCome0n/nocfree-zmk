#!/usr/bin/env bash
set -euo pipefail
export MSYS_NO_PATHCONV=1
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
if command -v cygpath >/dev/null 2>&1; then
  HOST_PATH="$(cygpath -w "$HERE")"
else
  HOST_PATH="$HERE"
fi
# LF line endings matter for scripts run under Linux containers
docker run --rm \
  -v "$HOST_PATH":/workspace \
  -w /workspace \
  zmkfirmware/zmk-build-arm:stable \
  bash /workspace/tools/inner_build_keepers.sh
