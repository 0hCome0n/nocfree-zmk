#!/usr/bin/env bash
# Build the NocFree ZMK firmware locally in Docker.
#
# Uses ZMK's own build image, which already contains the Zephyr SDK, west, and
# the ARM toolchain -- nothing to install on the host beyond Docker.
#
#   ./build.sh                         build left + right keepers (no dongle)
#   ./build.sh nocfree_left/...        build one fully-qualified board
#   ./build.sh --trial nocfree_right/...
#   ./build.sh --diag  nocfree_right/...   # implies --trial, never optional
#   ./build.sh --shell                 drop into the container
#
# The west workspace (zmk/, zephyr/, modules/, optional/) is checked out into the
# repo root and gitignored. First run downloads a few hundred MB; after that it is
# incremental.
#
# HARD GATE (2026-08-09 brick): any overlay that changes boot-path behaviour
# (diag.conf, or any EXTRA conf) is trial-only. The dongle is not built by
# default -- its topology is wrong (see PLAN.md) and flashing it is a brick risk.
set -euo pipefail

IMAGE="zmkfirmware/zmk-build-arm:stable"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Git Bash on Windows rewrites container-side paths: "-w /workspace" becomes
# "C:/Program Files/Git/workspace" and docker rejects it. MSYS_NO_PATHCONV stops
# that, and the volume source has to be a Windows-style path.
export MSYS_NO_PATHCONV=1
HOST_PATH="$HERE"
if command -v cygpath >/dev/null 2>&1; then
    HOST_PATH="$(cygpath -w "$HERE")"
fi

# Default: the two halves only. The dongle build is the leftover WRONG topology
# (dongle-as-central). Do not flash it. Build it explicitly if you need sizes.
BOARDS=("nocfree_left/nrf52833/zmk" "nocfree_right/nrf52833/zmk")

if [[ "${1:-}" == "--shell" ]]; then
    exec docker run --rm -it -v "$HOST_PATH":/workspace -w /workspace "$IMAGE" bash
fi

TRIAL=""
DIAG=""
SUFFIX=""
EXTRA_CONF=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --trial)
            TRIAL=1
            shift
            ;;
        --diag)
            # Diag is NEVER a keeper. The brick was a diag flash without trial.
            DIAG=1
            TRIAL=1
            shift
            ;;
        --with-dongle)
            BOARDS=("nocfree_dongle/nrf52833/zmk" "nocfree_left/nrf52833/zmk" "nocfree_right/nrf52833/zmk")
            shift
            ;;
        --help|-h)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        -*)
            echo "unknown flag: $1" >&2
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

if [[ $# -gt 0 ]]; then
    BOARDS=("$@")
fi

# Refuse to build if diag.conf still ACTIVELY enables the brick combo
# (commented lines do not count — only bare assignment lines).
if grep -E '^CONFIG_LOG_MODE_IMMEDIATE=y' "$HERE/diag.conf" 2>/dev/null | grep -q . && \
   grep -E '^CONFIG_LOG_BACKEND_UART=y' "$HERE/diag.conf" 2>/dev/null | grep -q .; then
    echo "REFUSING: diag.conf still enables LOG_MODE_IMMEDIATE + LOG_BACKEND_UART." >&2
    echo "That combination bricked the right half on 2026-08-09. See diag.conf.BRICKED-2026-08-09." >&2
    exit 1
fi

if [[ -n "$DIAG" ]]; then
    EXTRA_CONF="/workspace/diag.conf"
    SUFFIX="_DIAG"
fi

if [[ -n "$TRIAL" ]]; then
    # trial.conf then verify.conf: verify.conf widens the autodfu window to
    # 300 s so EVERY *_TRIAL.uf2 artifact has the same window regardless of
    # which entry point built it (this used to emit 120 s images under the
    # same names the 300 s inner_build_trials flow uses — an operator
    # following restore_split.ps1's 300 s assumption got mid-pass DFU drops).
    if [[ -n "$EXTRA_CONF" ]]; then
        EXTRA_CONF="${EXTRA_CONF};/workspace/trial.conf;/workspace/verify.conf"
    else
        EXTRA_CONF="/workspace/trial.conf;/workspace/verify.conf"
    fi
    SUFFIX="${SUFFIX}_TRIAL"
fi

# Compose the west -D flag. Zephyr accepts semicolon-separated EXTRA_CONF_FILE.
WEST_EXTRA=""
if [[ -n "$EXTRA_CONF" ]]; then
    WEST_EXTRA="-DEXTRA_CONF_FILE=${EXTRA_CONF}"
fi

echo "== boards: ${BOARDS[*]}"
echo "== trial:  ${TRIAL:-no}   diag: ${DIAG:-no}"
echo "== extra:  ${EXTRA_CONF:-none}"
echo

docker run --rm -v "$HOST_PATH":/workspace -w /workspace "$IMAGE" bash -euo pipefail -c '
    export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

    # west init -l config makes the REPO ROOT the topdir, because west uses the
    # parent of the manifest directory. So zmk/, zephyr/, modules/ and optional/
    # land here. They are gitignored. Do not redirect this into a subdirectory --
    # the build then cannot find zmk/app.
    # (No apostrophes in this block: the whole script body is single-quoted.)
    if [ ! -d .west ]; then
        echo "== west init (first run) =="
        west init -l config
    fi

    echo "== west update =="
    west update --narrow -o=--depth=1
    west zephyr-export

    for board in '"${BOARDS[*]}"'; do
        echo
        echo "=================================================================="
        echo "== building $board"
        echo "=================================================================="
        # "nocfree_left/nrf52833/zmk" -> "nocfree_left" for artifact naming
        name="${board%%/*}"
        SUFFIX="'"$SUFFIX"'"
        EXTRA="'"$WEST_EXTRA"'"

        # Refuse the wrong-topology dongle as a non-trial flashable artifact.
        if [ "$name" = "nocfree_dongle" ] && [ -z "'"$TRIAL"'" ]; then
            echo "!! nocfree_dongle is the leftover WRONG topology (dongle-central)."
            echo "!! Refusing keeper build. Use: ./build.sh --trial --with-dongle"
            echo "!! or build phase-2 bridge when it exists. See PLAN.md."
            exit 1
        fi

        # cmake-only first, then mkdir generated dirs, then compile.
        # On Docker Desktop + Windows bind mounts, parse_syscalls.py can
        # FileNotFoundError writing syscalls.json if the parent is missing
        # for a race after pristine. See tools/rebuild_keepers.sh.
        west build -p -s zmk/app -b "$board" \
            -d "build/$board" --cmake-only \
            -- -DZMK_CONFIG=/workspace/config $EXTRA || {
                echo "!! $board cmake FAILED"
                exit 1
            }
        mkdir -p "build/$board/zephyr/misc/generated" \
                 "build/$board/zephyr/include/generated"
        west build -d "build/$board" || {
                echo "!! $board compile FAILED"
                exit 1
            }

        # Mechanical gate: a non-trial image must not have trial autodfu, and a
        # trial image MUST have the watchdog-armed autodfu. Fail the build if not.
        CFG="build/$board/zephyr/.config"
        if [ -n "'"$TRIAL"'" ]; then
            if ! grep -q "^CONFIG_NOCFREE_TRIAL_AUTODFU=y" "$CFG"; then
                echo "!! TRIAL build missing CONFIG_NOCFREE_TRIAL_AUTODFU=y -- refusing artifact"
                exit 1
            fi
        else
            if grep -q "^CONFIG_NOCFREE_TRIAL_AUTODFU=y" "$CFG"; then
                echo "!! keeper build has TRIAL autodfu enabled -- refusing (would DFU every 120s)"
                exit 1
            fi
            # Keeper must never ship immediate UART logging into CDC.
            if grep -q "^CONFIG_LOG_MODE_IMMEDIATE=y" "$CFG" && \
               grep -q "^CONFIG_LOG_BACKEND_UART=y" "$CFG"; then
                echo "!! keeper has LOG_MODE_IMMEDIATE + LOG_BACKEND_UART -- that is the brick combo"
                exit 1
            fi
        fi

        # Recovery seatbelt must be present on every image.
        if ! grep -q "^CONFIG_NOCFREE_USB_RECOVERY=y" "$CFG"; then
            echo "!! CONFIG_NOCFREE_USB_RECOVERY missing -- refusing artifact"
            exit 1
        fi

        if [ -f "build/$board/zephyr/zmk.uf2" ]; then
            cp "build/$board/zephyr/zmk.uf2" "/workspace/${name}${SUFFIX}.uf2"
            echo "-> ${name}${SUFFIX}.uf2"
        elif [ -f "build/$name/zephyr/zmk.uf2" ]; then
            cp "build/$name/zephyr/zmk.uf2" "/workspace/${name}${SUFFIX}.uf2"
            echo "-> ${name}${SUFFIX}.uf2"
        elif [ -f "build/$name/zephyr/zephyr.uf2" ]; then
            cp "build/$name/zephyr/zephyr.uf2" "/workspace/${name}${SUFFIX}.uf2"
            echo "-> ${name}${SUFFIX}.uf2"
        else
            echo "!! no uf2 produced for $board"
            exit 1
        fi

        # Also emit the serial-DFU zip recipe inputs are already .hex next to uf2
        if [ -f "build/$board/zephyr/zmk.hex" ]; then
            cp "build/$board/zephyr/zmk.hex" "/workspace/${name}${SUFFIX}.hex"
        fi
    done
'

echo
echo "== artifacts =="
ls -la "$HERE"/*.uf2 2>/dev/null || echo "  no .uf2 produced"
echo
echo "Flash discipline:"
echo "  1. After unbrick: flash stock first (your stock backup) to prove HW."
echo "  2. Then *_TRIAL.uf2 only. Wait for self-return to bootloader (~120s) or WDT."
echo "  3. Only then flash the keeper (no _TRIAL / no _DIAG suffix)."
echo "  4. NEVER flash *DIAG* without _TRIAL in the name. build.sh will not emit one."
