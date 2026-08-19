# Tools

These are not optional extras — the recovery procedure and the success criteria
both depend on them, and they were previously living only in a session scratchpad.

| Tool | What it is for |
|---|---|
| `dfu_touch.py` | Drops a device into its bootloader over USB (1200-baud DTR touch). **This is the recovery path** for the dongle (no keys) and the halves (no reset pinhole). Verified working against stock firmware. |
| `uf2_rescue.py` | Turns a bootloader read-back (`CURRENT.UF2`) into a flashable restore image: restamps the UF2 family id the bootloader will accept, and drops the SoftDevice and the settings partition. Use it to snapshot a working build of *this* firmware before changing it. **It cannot capture stock firmware** — a stock device's touch gives serial-only DFU with no drive to read from (see `docs/DONGLE_SAFETY.md`). Never share a `CURRENT.UF2`: it contains your Bluetooth pairing keys. |
| `stickwatch2.py` | Logs key down/up transitions to measure dropped keystrokes. Three ways to stop it, none needing the keyboard. |
| `analyze.py` | Scores a stickwatch log for per-hand loss. Auto-detects the pangram drill. |

## Measuring whether the port worked

Baseline from the stock firmware, two runs at ~7 keys/s:

| | Loss |
|---|---|
| Left half | 2.9% |
| Right half | **7.4%** |

Same protocol after flashing, or the comparison is meaningless: `stickwatch2.py 120`,
pangram at your natural speed, then `analyze.py stickwatch.log`. Typing slowly hides
the fault — a clean result below ~5 keys/s proves nothing.
