# Dongle flash safety — DO NOT BRICK

The dongle has **no buttons**, **no reset pinhole**, and **must not be opened**.
Losing the software path into the Adafruit bootloader = permanently dead 2.4G
radio until someone opens the case (which we will not do).

This file is mandatory reading before any dongle flash.

## Retreat image — decide about this BEFORE you flash

The vendor's stock dongle firmware is **not distributed here** (it's their
copyrighted image), and NocFree does not publish it openly either. A retreat
image is therefore something you have to arrange for yourself, and — contrary to
what you might expect — you cannot simply read it off the device. Sort this out
before the first flash, because afterwards there is nothing left to capture.

### Read this part carefully: you probably cannot dump the stock firmware

It would be convenient if the bootloader could hand you a copy of what is
installed. On this hardware, **it cannot — not while stock firmware is on it.**

The stock firmware's 1200-baud touch writes DFU magic `0x4e`, which starts the
Adafruit bootloader in **serial-only** mode: you get a COM port, and no drive.
The mass-storage drive (the thing that carries `CURRENT.UF2`, the read-back)
only appears under magic `0x57`, which is what *this* project's firmware writes.
So the read-back becomes available only **after** you have already overwritten
stock — which is exactly too late. And the serial DFU protocol on the other side
of that COM port only writes; it has no read-back command.

Practically, that leaves three options, and you should pick one deliberately
before touching the dongle:

1. **Get the vendor's image.** NocFree does not publish it openly, but their
   firmware files do circulate (support, update packages, community). If you can
   obtain the `.uf2` for your dongle's version, that is your retreat — verify it
   with `tools/uf2check.py` and record its SHA256.
2. **Read the flash over SWD.** Complete and reliable, and out of scope here: it
   needs a debug probe and access to the pads, which for the dongle means
   opening the case. This project does not do that.
3. **Accept that it is one-way.** Flashing the dongle without a stock image means
   you may not be able to restore vendor behaviour later. That can be a perfectly
   reasonable trade — just make it knowingly, not by assuming a backup exists.

> **Backup and brick are different risks.** Not having a stock image does not
> make the dongle easier to brick; the trial-first ladder below is what protects
> against that, and it does not depend on a backup. What a stock image buys you
> is the ability to go *back*. Weigh them separately.

### After you are on this firmware, you can snapshot it

Once this project's firmware is installed, the touch gives you the UF2 drive, and
that drive holds `CURRENT.UF2` — a read-back of what is currently flashed. That
is worth copying before you change anything (upgrading, experimenting), and it is
how you snapshot a half.

```powershell
python tools\dfu_touch.py COMxx --watch      # drive appears
Copy-Item D:\CURRENT.UF2 .\my_backup.uf2
Get-FileHash .\my_backup.uf2 -Algorithm SHA256
```

It is a snapshot of *this* firmware, not of stock. It does not substitute for
option 1 above.

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
python tools/uf2_rescue.py my_backup.uf2 restore.uf2
# then flash restore.uf2 like any other image
```

## Going back to stock

If you have obtained a vendor image, restoring it is the *easy* direction —
easier than getting here was. The vendor's files are ordinary UF2s: checked
against real ones, they carry family id `0x621e937a` (the same one this board's
bootloader accepts, and the same our own builds use) and load at `0x27000`, with
no bad blocks. That means no special tooling — it is the same drag-and-drop as
any other image here.

```powershell
# 1. from THIS firmware, enter the bootloader (a drive appears)
python tools\dfu_touch.py COMxx --watch

# 2. copy the vendor image onto that drive
Copy-Item .\NocFree_..._Dongle.uf2 D:\
```

The flashers only know about the images bundled in `firmware/`, so a vendor
image is a manual copy rather than `flash.ps1 dongle`. Everything else behaves
the same: the board reboots itself when the last block lands, and the drive
disappearing is what tells you it took.

**Get the right file.** Vendor images are per device *and* per layout *and* per
version — a real set looks like `NocFree_and_V2.3.0_Dongle.uf2`,
`..._Left_ANSI.uf2`, `..._Right_ANSI.uf2`. Flashing a left image to a right half
will not brick it (both recover), but it will not work either. Verify what you
have first:

```bash
python tools/uf2check.py NocFree_and_V2.3.0_Dongle.uf2
# expect: bad 0, family 0x621e937a, flash 0x27000 .. ...
```

### The return trip is the hard one

Restoring stock closes the easy door behind you. Stock firmware writes DFU magic
`0x4e`, so from then on the 1200-baud touch gives a **serial-only** bootloader —
a COM port and no drive — and drag-and-drop is no longer available. Coming back
to this firmware then needs the serial DFU route:

```bash
adafruit-nrfutil dfu genpkg --dev-type 0x0052 --sd-req 0x0123 \
    --application zmk.hex out.zip
adafruit-nrfutil dfu serial --package out.zip -p COMxx -b 115200 --singlebank
```

That is the path this project used for its own first flash, so it is known to
work — but it is a different toolchain, and worth knowing about *before* you
decide to go back rather than after.

**One caveat, honestly flagged as unverified:** flashing an application does not
erase the settings partition, so a restored stock firmware starts life with this
project's leftover NVS still sitting at `0x6c000`. Stock most likely ignores or
reformats it, but that has not been tested here. If a restored dongle behaves
strangely, that leftover region is the first thing to suspect.

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
| `python tools/dfu_touch.py <COM> --watch` on stock | Works, but lands in **serial DFU** (magic `0x4e`): you get a COM port, *not* a drive. `--watch` will report no new drive, and that is the expected result on stock |
| UF2 mass-storage drive | Only under magic `0x57` — i.e. after this project's firmware is installed. This is the drag-and-drop path, and the one a vendor image is restored through |
| `adafruit-nrfutil` serial DFU | The write path when only a CDC bootloader appears — which is the stock case, so this is how stock → this firmware is done |
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
3. If bootloader/UF2 (a drive is mounted) → you are in the safe state. Copy a
   known-good image onto it: this project's `firmware/nocfree_dongle_BRIDGE.uf2`,
   or a vendor image if you have one (see "Going back to stock" above).  
4. If stock COM (8029) → stock is already running; nothing needs recovering.  
5. If **no USB at all after ~3 minutes** → WDT should have fired; replug again.  
6. If still nothing → stop. Do not try more images. Document and escalate;
   physical recovery is last resort and out of policy for this project.
