# NocFree "&" — ZMK port

A [ZMK](https://zmk.dev) firmware port for the **NocFree &** split keyboard
(nRF52833 halves), including a **custom BLE-HOG → USB dongle bridge** so the
board keeps all three of its output modes under ZMK: wired USB, direct
Bluetooth, and a 2.4 GHz dongle.

> ## ⚠️ Use entirely at your own risk
>
> This is a personal hobby project shared as-is. **I provide no warranty, no
> guarantee that it works, and no support of any kind.** I am under no
> obligation to respond to issues, questions, or pull requests, and nothing
> here is promised to be maintained, correct, or safe for your hardware.
>
> Flashing custom firmware **can permanently brick your keyboard or dongle**
> (the dongle especially — it has no buttons and no reset pinhole; see
> [`docs/DONGLE_SAFETY.md`](docs/DONGLE_SAFETY.md)). You alone are responsible
> for anything that happens to your devices, your data, or anything else. If
> you are not comfortable recovering a bricked nRF52 device yourself, **do not
> flash this.**
>
> Not affiliated with, endorsed by, or supported by the manufacturer. All
> trademarks belong to their respective owners. By using anything in this
> repository you accept full responsibility for the outcome. See the
> [MIT license](LICENSE) — note in particular the "AS IS", NO-WARRANTY clause.

## What works

- **Three output modes**, selected by the physical slide switch:
  - **USB** — wired HID
  - **Bluetooth** — direct BLE to the host (multi-profile)
  - **2.4 GHz** — via the dongle bridge (see below)
- **Split** left/right link (left is the central, right the peripheral)
- **ZMK Studio** on the left half (live keymap editing)
- Backlight with left→right sync, idle-off, and session-aware deep sleep
- Battery reporting (host shows the weaker half)
- Soft-off, deep sleep, and a software recovery path into the UF2 bootloader

## The dongle bridge (the interesting part)

ZMK is **BLE-only** — it has no support for the proprietary 2.4 GHz (Nordic
ESB) protocol the stock dongle uses, so the stock dongle can't talk to a ZMK
keyboard at all. This port solves that with a bridge instead:

```
right half  --BLE split-->  left half  --BLE HOG-->  dongle  --USB HID-->  host
(peripheral)                (central +              (BLE central +
                             HID peripheral)         HID-over-GATT client)
```

The dongle runs custom firmware (`dongle_bridge/`) as a **BLE HOG central**: it
connects to the left half (which advertises as a BLE HID peripheral), subscribes
to its HID reports, and re-presents them to the host as USB HID. So "2.4 GHz
mode" is really just another BLE link that happens to live in the 2.4 GHz band —
but it keeps the split intact and gives you a plug-and-go USB receiver.

This is **not** ZMK's standard "dongle-as-split-central" topology; the left
stays the split central and the dongle is a *second* BLE central hanging off it.
See [`docs/BRIDGE.md`](docs/BRIDGE.md).

## Repository layout

| Path | What |
|------|------|
| `flash.ps1` / `flash.sh` | One-command flasher — Windows / macOS-Linux (no Python) |
| `firmware/` | Prebuilt keeper UF2s (flash without building) |
| `config/` | ZMK board definition (dts, keymap, Kconfig, board C sources) |
| `dongle_bridge/` | Standalone bridge firmware for the dongle |
| `patches/` | Local ZMK patches (applied to the `zmk/` tree at build time) |
| `tools/` | Build (Docker) and flash helpers |
| `docs/` | Bridge design + dongle flashing safety |
| `.github/workflows/` | ZMK GitHub Actions build for the two halves |

## Building

**The two halves** build with the standard ZMK GitHub Action (see
`build.yaml` / `.github/workflows/build.yml`) — fork this repo and let CI build
them, or build locally.

**The dongle bridge** is a separate Zephyr app and builds via the Docker
scripts in `tools/` (it is *not* built by the ZMK Action):

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace \
  zmkfirmware/zmk-build-arm:stable \
  bash tools/inner_build_dongle_bridge_trial.sh   # trial (safe: auto-DFU)
```

Local half builds use the same image with `tools/inner_build_left_keeper.sh` /
`inner_build_right_keeper.sh`. On Windows, run the Docker commands from
PowerShell (Git Bash mangles the `-w /workspace` path).

## Flashing

Prebuilt keeper firmware is in [`firmware/`](firmware/) — you don't have to
build anything to flash. All devices use the Adafruit nRF52 UF2 bootloader,
entered with a **1200-baud touch** (no buttons needed).

**Windows — `flash.ps1` (no Python, no installs; just stock PowerShell):**

```powershell
.\flash.ps1 left       # finds the device, enters bootloader, copies firmware
.\flash.ps1 right
.\flash.ps1 dongle     # prompts for confirmation first -- see the warning below
```

**macOS / Linux — `flash.sh` (no Python; plug in only the target device):**

```bash
./flash.sh left
./flash.sh right
./flash.sh dongle      # prompts for confirmation first
```

> `flash.sh` was developed on Windows and is **untested on real Mac/Linux
> hardware** — watch it on first use. If it can't find the device or the
> volume, fall back to the fully manual method: trigger the bootloader with a
> 1200-baud open, then copy the file onto the drive that appears.
>
> ```bash
> # Linux:  stty -F /dev/ttyACM0 1200
> # macOS:  stty -f  /dev/cu.usbmodemXXXX 1200
> cp firmware/nocfree_left.uf2 /path/to/NOCFREE_BOOT/
> ```

Suggested order: **right → left → dongle**.

> ### The dongle is opt-in and gated on purpose
> The dongle can be **bricked permanently** — no buttons, no reset pinhole, so
> its only recovery is the software touch. `flash.ps1 dongle` therefore makes
> you type a confirmation first, and you should **back up your own stock dongle
> firmware and read [`docs/DONGLE_SAFETY.md`](docs/DONGLE_SAFETY.md) before you
> do it.** Vendor firmware is not distributed here — that backup is on you.

### ⚠️ Read this before flashing the dongle

The dongle has **no buttons and no reset pinhole**. Its only recovery path is
the software 1200-baud touch, so a firmware that hangs before USB enumerates can
brick it permanently. **Dump and keep a backup of your dongle's stock firmware
before you flash anything**, and follow the trial-first ladder in
[`docs/DONGLE_SAFETY.md`](docs/DONGLE_SAFETY.md). Vendor firmware is **not**
distributed here — you must back up your own.

## Status

Daily-driver stable. Reverse-engineering notes and vendor firmware used during
development are intentionally not included in this repository.

## License

Original port and bridge code are MIT ([`LICENSE`](LICENSE)). ZMK and Zephyr
remain under their own licenses; no proprietary vendor firmware is included.
