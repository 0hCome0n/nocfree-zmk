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
  Darwin)
    ports=(/dev/cu.usbmodem*)
    # macOS drops DTR when the last user of the port closes it.
    touch_cmd() { stty -f "$1" 1200; }
    ;;
  Linux)
    ports=(/dev/ttyACM*)
    # `hupcl` is REQUIRED: the reset trigger is the DTR *drop*, and on Linux DTR
    # is only lowered on close when HUPCL is set. `stty -F ... 1200` alone sets
    # the baud rate and resets nothing, which looks exactly like a dead script.
    touch_cmd() { stty -F "$1" 1200 hupcl; }
    ;;
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
# NUL-separated so volume names containing spaces (common on macOS) survive.
mount_roots() {
  case "$os" in
    Darwin) find /Volumes -maxdepth 1 -mindepth 1 -print0 2>/dev/null ;;
    Linux)  find /media /run/media /mnt -maxdepth 2 -mindepth 1 -print0 2>/dev/null ;;
  esac
}
# A UF2 volume identifies its board in INFO_UF2.TXT; ours says "NocFree &".
# Checking it stops a stray copy onto another UF2 board that is also mounted --
# that board would be broken by our firmware.
is_nocfree_uf2() { [ -f "$1/INFO_UF2.TXT" ] && grep -qi nocfree "$1/INFO_UF2.TXT" 2>/dev/null; }

before_list="$(mount_roots | tr '\0' '\n' || true)"
was_present() { printf '%s\n' "$before_list" | grep -Fxq "$1"; }

echo "$target is on $port; entering bootloader ..."
touch_cmd "$port" || true   # the port drops as the device resets; not an error

echo "waiting for the bootloader volume ..."
drive=""
for _ in $(seq 1 30); do
  while IFS= read -r -d '' m; do
    if was_present "$m"; then continue; fi
    if is_nocfree_uf2 "$m"; then drive="$m"; break; fi
  done < <(mount_roots)
  [ -n "$drive" ] && break
  sleep 0.5
done
if [ -z "$drive" ]; then
  # Maybe it was already in the bootloader before we started (cancelled attempt,
  # or a trial build that handed itself back) -- no new volume would appear.
  while IFS= read -r -d '' m; do
    if is_nocfree_uf2 "$m"; then drive="$m"; echo "using already-mounted bootloader volume $drive"; break; fi
  done < <(mount_roots)
fi
[ -n "$drive" ] || { echo "no bootloader volume appeared. Unplug/replug and retry; firmware untouched."; exit 1; }

echo "copying $uf2 -> $drive/ ..."
# The board reboots the moment the last block lands, so the volume can vanish
# mid-write and cp/sync may report an error after a perfectly good flash. The
# check below is what decides success, so do not let set -e abort on that.
cp "$fw" "$drive/" 2>/dev/null || echo "  (copy reported an error -- expected if the board rebooted mid-write; verifying)"
sync 2>/dev/null || true
sleep 5   # the board reboots itself once the UF2 lands
if [ -f "$drive/INFO_UF2.TXT" ]; then
  echo "still in the bootloader -- the copy may not have taken. Retry."; exit 1
fi
echo "done: $target flashed and rebooted."
