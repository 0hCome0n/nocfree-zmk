#!/usr/bin/env bash
# Easy flasher for the NocFree & ZMK firmware -- macOS and Linux. No Python.
#
#   ./flash.sh left
#   ./flash.sh right
#   ./flash.sh dongle     # prompts for confirmation first (see the warning)
#
# It drops the device into the UF2 bootloader with a 1200-baud touch (no
# buttons), waits for the bootloader volume to mount, copies the matching
# firmware/*.uf2 onto it, and confirms.
#
# !! UNTESTED ON REAL MAC/LINUX HARDWARE. This was developed on Windows and
#    mirrors the (working) flash.ps1 logic, but its first real run is yours.
#    Watch it, and fall back to the manual method in the README if it misbehaves.
#    Entering the bootloader erases nothing; the halves always recover.
#
# !! THE DONGLE CAN BE BRICKED PERMANENTLY (no buttons, no reset pinhole).
#    Flashing it requires typing a confirmation, and you should back up your own
#    stock dongle firmware and read docs/DONGLE_SAFETY.md FIRST.
#
# To avoid guessing which USB device is which, plug in ONLY the device you are
# flashing. The script uses the single serial port it finds.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

target="${1:-}"
case "$target" in
  left)   uf2="nocfree_left.uf2" ;;
  right)  uf2="nocfree_right.uf2" ;;
  dongle) uf2="nocfree_dongle_BRIDGE.uf2" ;;
  *) echo "usage: ./flash.sh left|right|dongle"; exit 1 ;;
esac

fw="$ROOT/firmware/$uf2"
[ -f "$fw" ] || { echo "firmware not found: $fw"; exit 1; }

if [ "$target" = "dongle" ]; then
  echo
  echo "  !! DONGLE FLASH -- READ THIS !!"
  echo "  The dongle has no buttons and no reset pinhole. A bad or interrupted"
  echo "  flash can brick it PERMANENTLY. Back up your own stock dongle firmware"
  echo "  and read docs/DONGLE_SAFETY.md first. This is entirely at your own risk."
  echo
  printf "  Type  I UNDERSTAND  to proceed (anything else cancels): "
  read -r reply
  [ "$reply" = "I UNDERSTAND" ] || { echo "Cancelled -- nothing was touched."; exit 0; }
fi

# --- find the serial device (expects exactly one plugged in) ---
os="$(uname -s)"
case "$os" in
  Darwin) ports=(/dev/cu.usbmodem*) ; touch_cmd() { stty -f "$1" 1200; } ;;
  Linux)  ports=(/dev/ttyACM*)      ; touch_cmd() { stty -F "$1" 1200; } ;;
  *) echo "unsupported OS '$os' -- use the manual method in the README"; exit 1 ;;
esac
# Filter to entries that actually exist (globs stay literal when nothing matches).
real=(); for p in "${ports[@]}"; do [ -e "$p" ] && real+=("$p"); done
if [ "${#real[@]}" -eq 0 ]; then
  echo "no NocFree serial device found. Is it plugged in and running?"; exit 1
elif [ "${#real[@]}" -gt 1 ]; then
  echo "more than one serial device found: ${real[*]}"
  echo "unplug the others and leave only the $target plugged in, then retry."
  exit 1
fi
port="${real[0]}"

# --- snapshot mounts, touch, wait for the bootloader volume ---
mount_roots() { case "$os" in Darwin) echo /Volumes/*;; Linux) echo /media/*/* /run/media/*/*;; esac; }
before="$(mount_roots 2>/dev/null || true)"

echo "$target is on $port; entering bootloader ..."
touch_cmd "$port" || true   # the port drops as the device resets; not an error

echo "waiting for the bootloader volume ..."
drive=""
for _ in $(seq 1 30); do
  for m in $(mount_roots 2>/dev/null || true); do
    case " $before " in *" $m "*) : ;; *)
      [ -f "$m/INFO_UF2.TXT" ] && { drive="$m"; break; } ;;
    esac
  done
  [ -n "$drive" ] && break
  sleep 0.5
done
[ -n "$drive" ] || { echo "no bootloader volume appeared. Unplug/replug and retry; firmware untouched."; exit 1; }

echo "copying $uf2 -> $drive/ ..."
cp "$fw" "$drive/" && sync
sleep 5   # the board reboots itself once the UF2 lands
if [ -f "$drive/INFO_UF2.TXT" ]; then
  echo "still in the bootloader -- the copy may not have taken. Retry."; exit 1
fi
echo "done: $target flashed and rebooted."
