# Dongle bridge — BLE HOG → USB HID

**Status: KEEPER LIVE** (`nocfree_dongle_BRIDGE.uf2`, PID `2886:9029`).
Keyboard + consumer HID proven. Not a Studio path (use left USB/BT).

## Topology

```
left half (ZMK central, HOG server)  --BLE-->  dongle (HOG client + USB HID)  -->  host/KVM
right half --split BLE--> left
```

Dongle is **not** a ZMK split central. It is a small Zephyr app in `dongle_bridge/`.

## Implemented
- Scan/connect/bond to `"NocFree &"` / HIDS
- Subscribe keyboard report ID 1 (8-byte boot body) → USB HID ID 1
- Subscribe consumer report ID 2 (6Ã—uint16 FULL) → USB HID ID 2
- Queued USB writes; release-all on disconnect
- HID-friendly conn interval; calm reconnect (soft ~15s / hard ~45s)
- USB recovery 1200-baud; keeper = no autodfu; RC LFCLK only

## Not implemented (low priority)
- LED output report host → left
- Host suspend polish
- XTAL clock (unproven; RC is fine on USB power)

## Safety
See `docs/DONGLE_SAFETY.md`. Stock retreat:
- `your stock dongle backup`
- your stock backup in serial-DFU form

## Build
`tools/inner_build_dongle_bridge_keeper.sh` (Docker).

## BLE contract (left HOG)
- Keyboard ID 1: mods + reserved + 6 keys
- Consumer ID 2: 6Ã—uint16 (CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_FULL)
