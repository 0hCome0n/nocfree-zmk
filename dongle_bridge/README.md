# NocFree dongle bridge (phase 1)

Standalone Zephyr app: BLE central (future HOG client) + USB HID + **mandatory**
1200-baud recovery + trial autodfu/WDT.

This is **not** the leftover ZMK `nocfree_dongle` board (wrong split-central
topology). Do not flash that board.

## Safety

Read `docs/DONGLE_SAFETY.md` first. Stock retreat:

- `your stock dongle backup`
- your stock backup in serial-DFU form

## Build (container only)

```bash
docker exec zmk-build-arm bash /workspace/tools/inner_build_dongle_bridge_trial.sh
```

Output: `nocfree_dongle_BRIDGE_TRIAL.uf2` + `.config` at repo root.

## Flash policy

Phase 1 produces a **trial** image only (120 s self-return + 180 s WDT).
Flashing is a **separate, deliberate step** after gates pass and stock restore
is staged. Do not automate flash in this repo for the dongle.
