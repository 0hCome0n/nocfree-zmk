# Dongle flash safety — DO NOT BRICK

The dongle has **no buttons**, **no reset pinhole**, and **must not be opened**.
Losing the software path into the Adafruit bootloader = permanently dead 2.4G
radio until someone opens the case (which we will not do).

This file is mandatory reading before any dongle flash.

## Retreat image — DUMP YOUR OWN BEFORE YOU FLASH

The vendor's stock dongle firmware is **not distributed here** (it's their
copyrighted image), and NocFree does not publish it either. So the only backup
you will ever have is the one you take off your own device — and you must take
it *before* you overwrite anything. Treat this as step zero.

**The bootloader can dump itself.** You do not need the vendor, a programmer, or
a debug probe. While a device sits in the UF2 bootloader, its drive holds a file
called `CURRENT.UF2`: a read-back of what is installed right now. Copy it off and
you have your retreat.

```powershell
# 1. put the device in the bootloader (this erases nothing)
python tools\dfu_touch.py            # find the port; dongle is VID 2886, PID 9029
python tools\dfu_touch.py COMxx --watch

# 2. copy the read-back off the drive that appears, and hash it
Copy-Item D:\CURRENT.UF2 .\my_stock_dongle_backup.uf2
Get-FileHash .\my_stock_dongle_backup.uf2 -Algorithm SHA256
```

Do this on the **stock** dongle, before its first ZMK flash. Once it is
overwritten, the stock image is gone and no backup can be taken retroactively.

### What is actually in that file

Verified by dumping one and taking it apart (nRF52833, bootloader 0.9.2):

| Region | Address range | What it is |
|---|---|---|
| SoftDevice | `0x01000`–`0x27000` | Nordic BLE stack. Untouched by an app flash |
| Application | `0x27000`–`0x6c000` | The firmware itself |
| Settings | `0x6c000`–`0x74000` | NVS: **Bluetooth bonds**, saved state |

Two consequences, both important:

> **⚠️ Never share `CURRENT.UF2`.** The settings partition holds live pairing
> material — a real dump shows `bt/keys/<paired device address>` next to the
> long-term keys. Posting your backup in a forum thread or a bug report hands
> over your Bluetooth link keys and the addresses of devices you have paired
> with. Keep it on your own disk.

> **It is a read-back, not the original image, and cannot be flashed back
> as-is.** The bootloader stamps its dumps with a different UF2 family id
> (`0x239a0029`) than the one it accepts for writes (`0x621e937a`), so a
> bootloader that checks will reject its own dump. `tools/uf2_rescue.py`
> converts it: it restamps the family id, keeps the application, and drops the
> SoftDevice (never erased, so never needs restoring) and the settings partition
> (per-device state, and the part carrying your keys).

```bash
python tools/uf2_rescue.py my_stock_dongle_backup.uf2 restore_stock.uf2
# then flash restore_stock.uf2 like any other image
```

### Verify the round trip before you need it

The read-back path is proven: dumping a device whose firmware was known, then
converting it, reproduced all 740 blocks of that firmware byte for byte. (The
dump also carried one extra block — the tail of a *larger* image flashed
earlier, since writing a UF2 never erases past what it writes.)

What has **not** been verified here is writing a converted image back onto a
device, because doing so on a dongle risks the very device that cannot be
recovered. So prove it on a **half** first — they recover from anything — by
dumping one, converting it, flashing the result, and confirming the keyboard
still works. Only rely on it for the dongle after you have seen it work.

## USB identities

| Firmware | VID:PID | Meaning |
|----------|---------|---------|
| Stock vendor | `2886:8029` | Known-good stock (retreat target) |
| ZMK bridge (future) | `2886:9029` | Our bridge app only |
| **Wrong** leftover ZMK dongle-central | `2886:9029` | **NEVER FLASH** (see below) |
| Adafruit bootloader | `239A:002A` (or UF2 mass storage) | Safe place to reflash |

## Absolute bans

1. **Never flash `nocfree_dongle` ZMK board as a keeper.**  
   `nocfree_dongle_nrf52833_zmk_defconfig` is the leftover **wrong topology**
   (dongle as split central). It can break half behavior and is not the bridge.
   `build.sh` refuses non-trial dongle keepers — leave that gate intact.

2. **Never flash an image that lacks:**
   - USB CDC + 1200-baud → UF2 bootloader (`recovery.c` or equivalent)
   - Trial autodfu + **PRE_KERNEL_1 WDT** for the first flash of any new app
   - Internal RC LFCLK until crystal is proven (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC`)  
     **XTAL on dongle is unverified.** Wrong XTAL hang can freeze before USB —
     recovery never arms → brick.

3. **Never flash over a live "maybe dead" path.** If CDC does not enumerate
   after a trial, wait for WDT (~WDT seconds), replug, look for bootloader
   VID, restore **stock** immediately.

4. **Never delete or overwrite stock UF2s** in `your-stock-backup/`.

## Recovery routes (dongle)

| Route | When it works |
|-------|----------------|
| `python tools/dfu_touch.py <COM> --watch` on stock | Proven: stock uses 1200-baud; may enter **serial DFU** (magic `0x4e`) not UF2 |
| UF2 mass-storage drive | After ZMK recovery (magic `0x57`) or if bootloader presents a drive |
| `adafruit-nrfutil` serial DFU | When only CDC bootloader appears (stock touch path) |
| Replug | Sometimes restarts app; does **not** fix a wedged no-USB image |
| Case open / pinhole | **Unavailable — do not rely on this** |

**Always keep stock ready** on the machine before any experimental flash:

```powershell
# After device is in bootloader / UF2 drive:
Copy-Item your_stock_dongle_backup.uf2 <DRIVE>:\
# Or serial DFU with adafruit-nrfutil if no drive (use the tool's documented nRF52 DFU flow)
```

## Mandatory flash order (every new dongle app)

```
0. Verify stock UF2 hash on disk
1. Plug dongle, confirm stock 2886:8029 (or known last-good)
2. Practice: dfu_touch → bootloader → flash STOCK over itself → back to 8029
   (zero risk; proves host tooling + cables)
3. Flash *_TRIAL only (recovery + autodfu + WDT, RC clock)
4. Observe: enumerates → self-returns to bootloader within trial window
5. Immediately restore STOCK from bootloader (prove retreat)
6. Only after N successful trial→stock cycles: consider a non-trial keeper
7. Never promote an image that cannot re-enter bootloader via dfu_touch
```

## What we are building (correct product)

See `docs/BRIDGE.md`:

```
left half (ZMK, BLE peripheral / HOG)  --BLE-->  dongle (BLE central + HIDS client)
                                                   |
                                                 USB HID --> host
```

- Left stays split central for the right half.
- Mode switch 2.4G already selects a dedicated BLE profile for the dongle.
- Dongle is **not** a ZMK split central.
- Stock dongle (encrypted ESB) remains the retreat until the bridge is proven.

## Phase checklist

| Phase | Goal | Flash dongle? |
|-------|------|----------------|
| 0 | Safety doc, stock hash, tooling check | No experimental — **DONE** |
| 0b | Stock self-reflash round-trip | Stock only — **DONE** |
| 1 | Bridge app skeleton + recovery + trial WDT, build size | No — **DONE** (`nocfree_dongle_BRIDGE_TRIAL.uf2`) |
| 2 | First TRIAL flash → auto bootloader → stock restore | Trial + stock only — **not started** |
| 3 | BLE connect/bond to left HOG, USB HID pass-through (trial) | Trial |
| 4 | Keeper bridge after multi-hour desk use | Only after 2+3 green |

### Phase 1 build command (no flash)

```bash
docker exec zmk-build-arm bash /workspace/tools/inner_build_dongle_bridge_trial.sh
```

Gates: recovery=y, trial autodfu=y, RC clock, no XTAL, no ZMK_SPLIT, flash < 270 KB.

## If something goes wrong

1. Unplug dongle.  
2. Replug. Note VID:PID.  
3. If bootloader/UF2 → flash `your stock dongle backup`.  
4. If stock COM (8029) → use `dfu_touch` then restore stock.  
5. If **no USB at all after ~3 minutes** → WDT should have fired; replug again.  
6. If still nothing → stop. Do not try more images. Document and escalate;
   physical recovery is last resort and out of policy for this project.
