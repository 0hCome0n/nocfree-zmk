#!/usr/bin/env bash
# Runs INSIDE the zmk-build-arm container.
#
# RETIRED AS AN INDEPENDENT BUILD PATH (2026-08-14): this script used to carry
# its own (weaker) copy of the safety gates and silently produced keeper
# artifacts without the Studio snippet or the link-timing / bond-flag gates.
# That is the exact stale-artifact incident class the per-side scripts were
# hardened against, so this is now a thin wrapper that delegates to them —
# the gates live in exactly one place per device.
set -euo pipefail

bash /workspace/tools/inner_build_right_keeper.sh
bash /workspace/tools/inner_build_left_keeper.sh

echo
echo "== done keepers (via gated per-side scripts) =="
ls -la /workspace/nocfree_left.uf2 /workspace/nocfree_right.uf2
