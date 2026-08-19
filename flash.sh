#!/usr/bin/env bash
# Easy flasher for the NocFree & ZMK firmware -- macOS and Linux. No Python.
#
#   ./flash.sh            # guided menu (start here)
#   ./flash.sh left
#   ./flash.sh right
#   ./flash.sh dongle     # requires typing a confirmation (see the warning)
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
# Unlike the Windows script, this cannot tell the halves apart by USB id, so it
# uses the single serial port it finds: plug in ONLY the device being flashed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

os="$(uname -s)"
case "$os" in
  Darwin)
    port_glob=(/dev/cu.usbmodem*)
    # macOS drops DTR when the last user of the port closes it.
    touch_cmd() { stty -f "$1" 1200; }
    ;;
  Linux)
    port_glob=(/dev/ttyACM*)
    # `hupcl` is REQUIRED: the reset trigger is the DTR *drop*, and on Linux DTR
    # is only lowered on close when HUPCL is set. `stty -F ... 1200` alone sets
    # the baud rate and resets nothing, which looks exactly like a dead script.
    touch_cmd() { stty -F "$1" 1200 hupcl; }
    ;;
  *) echo "unsupported OS '$os' -- use the manual method in the README"; exit 1 ;;
esac

uf2_for() {
  case "$1" in
    left)   echo "nocfree_left.uf2" ;;
    right)  echo "nocfree_right.uf2" ;;
    dongle) echo "nocfree_dongle_BRIDGE.uf2" ;;
  esac
}

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

find_ports() {
  local p
  for p in "${port_glob[@]}"; do [ -e "$p" ] && printf '%s\n' "$p"; done
}

find_boot_volume() {
  local m
  while IFS= read -r -d '' m; do
    if is_nocfree_uf2 "$m"; then printf '%s\n' "$m"; return 0; fi
  done < <(mount_roots)
  return 1
}

show_devices() {
  echo
  echo "  Connected devices"
  local ports vol
  ports="$(find_ports || true)"
  if [ -n "$ports" ]; then
    while IFS= read -r p; do echo "    serial port   $p"; done <<< "$ports"
  else
    echo "    serial port   none detected"
  fi
  if vol="$(find_boot_volume)"; then
    echo "    bootloader    $vol (waiting for firmware)"
  fi
  echo
}

# Returns 0 on success, 1 on failure -- never exits, so the menu can continue.
do_flash() {
  local target="$1"
  local uf2 fw port drive before_list
  uf2="$(uf2_for "$target")"
  fw="$ROOT/firmware/$uf2"
  if [ ! -f "$fw" ]; then echo "firmware not found: $fw"; return 1; fi

  if [ "$target" = "dongle" ]; then
    echo
    echo "  !! DONGLE FLASH -- READ THIS !!"
    echo "  The dongle has no buttons and no reset pinhole. A bad or interrupted"
    echo "  flash can brick it PERMANENTLY. Back up your own stock dongle firmware"
    echo "  and read docs/DONGLE_SAFETY.md first. This is entirely at your own risk."
    echo
    printf "  Type  I UNDERSTAND  to proceed (anything else cancels): "
    read -r reply
    [ "$reply" = "I UNDERSTAND" ] || { echo "Cancelled -- nothing was touched."; return 1; }
  fi

  before_list="$(mount_roots | tr '\0' '\n' || true)"
  was_present() { printf '%s\n' "$before_list" | grep -Fxq "$1"; }

  local ports n
  ports="$(find_ports || true)"
  n="$(printf '%s' "$ports" | grep -c . || true)"
  drive=""

  if [ "$n" -eq 0 ]; then
    # No serial port: maybe it is already sitting in the bootloader (cancelled
    # attempt, or a trial image that handed itself back).
    if drive="$(find_boot_volume)"; then
      echo "no serial device, but a NocFree bootloader volume is mounted at $drive -- using it."
    else
      echo "no NocFree serial device found. Is it plugged in and running?"; return 1
    fi
  elif [ "$n" -gt 1 ]; then
    echo "more than one serial device found:"; printf '  %s\n' $ports
    echo "unplug the others and leave only the $target plugged in, then retry."
    return 1
  else
    port="$ports"
    echo "$target is on $port; entering bootloader ..."
    touch_cmd "$port" || true   # the port drops as the device resets; not an error

    echo "waiting for the bootloader volume ..."
    local m
    for _ in $(seq 1 30); do
      while IFS= read -r -d '' m; do
        if was_present "$m"; then continue; fi
        if is_nocfree_uf2 "$m"; then drive="$m"; break; fi
      done < <(mount_roots)
      [ -n "$drive" ] && break
      sleep 0.5
    done
    if [ -z "$drive" ]; then
      # It may have been in the bootloader all along, so no NEW volume appears.
      drive="$(find_boot_volume || true)"
      [ -n "$drive" ] && echo "using already-mounted bootloader volume $drive"
    fi
  fi

  if [ -z "$drive" ]; then
    echo "no bootloader volume appeared. Unplug/replug and retry; firmware untouched."
    return 1
  fi

  echo "copying $uf2 -> $drive/ ..."
  # The board reboots the moment the last block lands, so the volume can vanish
  # mid-write and cp/sync may report an error after a perfectly good flash. The
  # check below is what decides success, so do not let set -e abort on that.
  cp "$fw" "$drive/" 2>/dev/null || echo "  (copy reported an error -- expected if the board rebooted mid-write; verifying)"
  sync 2>/dev/null || true
  sleep 5   # the board reboots itself once the UF2 lands
  if [ -f "$drive/INFO_UF2.TXT" ]; then
    echo "still in the bootloader -- the copy may not have taken. Retry."; return 1
  fi
  echo "done: $target flashed and rebooted."
  return 0
}

# --- direct mode: a target was named ----------------------------------------
target="${1:-}"
case "$target" in
  left|right|dongle) do_flash "$target" && exit 0 || exit 1 ;;
  "") : ;;   # fall through to the menu
  *) echo "usage: ./flash.sh [left|right|dongle]   (no argument = guided menu)"; exit 1 ;;
esac

# --- guided mode -------------------------------------------------------------
echo
echo "Plug in ONE device at a time -- this script cannot tell the halves apart"
echo "by USB id, so it uses whichever serial port it finds. Entering the"
echo "bootloader erases nothing, and you can quit at any prompt."

while true; do
  echo
  echo "  NocFree & -- ZMK firmware flasher"
  echo "  ---------------------------------"
  show_devices
  echo "  What would you like to flash?"
  echo "    1) Right half      (do this one first)"
  echo "    2) Left half"
  echo "    3) Dongle          (brick risk -- read docs/DONGLE_SAFETY.md first)"
  echo "    r) Re-scan devices"
  echo "    q) Quit            (nothing has been touched)"
  echo
  printf "  Choice: "
  read -r choice
  case "$(printf '%s' "$choice" | tr '[:upper:]' '[:lower:]')" in
    1) do_flash right || true ;;
    2) do_flash left   || true ;;
    3) do_flash dongle || true ;;
    r) : ;;
    q) echo "Bye."; exit 0 ;;
    *) echo "  Please choose 1, 2, 3, r or q." ;;
  esac
done
