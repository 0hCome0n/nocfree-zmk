#!/usr/bin/env bash
# Runs INSIDE zmk-build-arm. Builds the dongle BLE->USB bridge KEEPER image.
# NEVER flashes. See docs/DONGLE_SAFETY.md.
set -euo pipefail
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
cd /workspace
west zephyr-export

name=nocfree_dongle_BRIDGE
bdir=/tmp/zmk-build/dongle_bridge_keeper
echo "== dongle bridge KEEPER build (out=$bdir) =="
rm -rf "$bdir"
mkdir -p "$bdir"

west build -p -s /workspace/dongle_bridge -b nocfree_bridge/nrf52833 -d "$bdir" \
  -- -DEXTRA_CONF_FILE=/workspace/dongle_bridge/prj_keeper.conf

CFG="$bdir/zephyr/.config"
UF2="$bdir/zephyr/zephyr.uf2"
if [[ ! -f "$UF2" ]]; then
  UF2="$bdir/zephyr/zmk.uf2"
fi
if [[ ! -f "$UF2" ]]; then
  UF2=$(ls "$bdir"/zephyr/*.uf2 2>/dev/null | head -1 || true)
fi
[[ -n "${UF2:-}" && -f "$UF2" ]] || { echo "!! no UF2 produced"; ls -la "$bdir/zephyr/" | head -40; exit 1; }

# --- hard safety gates ---
grep -q '^CONFIG_NOCFREE_USB_RECOVERY=y' "$CFG" || { echo "!! no USB recovery"; exit 1; }
grep -q '^CONFIG_NOCFREE_BRIDGE_KEEPER=y' "$CFG" || { echo "!! keeper flag missing"; exit 1; }
if grep -q '^CONFIG_NOCFREE_TRIAL_AUTODFU=y' "$CFG"; then
  echo "!! trial autodfu must NOT be set on keeper"; exit 1
fi
grep -q '^CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y' "$CFG" || { echo "!! RC clock required (no XTAL on dongle yet)"; exit 1; }
if grep -q '^CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y' "$CFG"; then
  echo "!! XTAL enabled on dongle — unverified, refuse"; exit 1
fi
if grep -q '^CONFIG_LOG_MODE_IMMEDIATE=y' "$CFG" && grep -q '^CONFIG_LOG_BACKEND_UART=y' "$CFG"; then
  echo "!! brick combo"; exit 1
fi
if grep -q '^CONFIG_ZMK_SPLIT=y' "$CFG" 2>/dev/null; then
  echo "!! ZMK_SPLIT set — wrong app/topology"; exit 1
fi

SIZE=$(stat -c%s "$UF2")
if [[ "$SIZE" -gt 600000 ]]; then
  echo "!! UF2 suspiciously large ($SIZE)"; exit 1
fi
BIN="$bdir/zephyr/zephyr.bin"
if [[ -f "$BIN" ]]; then
  BSIZE=$(stat -c%s "$BIN")
  echo "bin size=$BSIZE bytes  UF2 size=$SIZE bytes"
  if [[ "$BSIZE" -gt 270000 ]]; then
    echo "!! binary near/over 276KB code partition ($BSIZE)"; exit 1
  fi
else
  echo "UF2 size=$SIZE bytes (no .bin to size-check)"
fi

mkdir -p /workspace/releases
cp "$UF2" "/workspace/${name}.uf2"
cp "$CFG" "/workspace/${name}.config"
cp "$bdir/zephyr/zephyr.hex" "/workspace/${name}.hex" 2>/dev/null || true
# Rename on copy — a bare multi-source cp kept the zephyr.uf2/.config
# basenames, so releases/ never got the keeper name (2026-08-12 stale-flash).
cp "$UF2" "/workspace/releases/${name}.uf2"
cp "$CFG" "/workspace/releases/${name}.config"
cp "$bdir/zephyr/zephyr.hex" "/workspace/releases/${name}.hex" 2>/dev/null || true
echo "-> ${name}.uf2 ($SIZE bytes) + releases/"
echo "SAFETY: KEEPER (no autodfu). Keep USB recovery + stock retreat ready."
echo "  Retreat: your own dumped stock dongle firmware (back it up BEFORE flashing)"
echo "  Recovery: python tools/dfu_touch.py COMxx"
echo "== dongle bridge keeper build done (NOT FLASHED) =="
