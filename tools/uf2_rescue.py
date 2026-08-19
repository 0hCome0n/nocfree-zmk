"""Turn a bootloader read-back (CURRENT.UF2) into a flashable restore image.

WHY THIS EXISTS
---------------
The vendor does not distribute the stock firmware, so the only backup you can
ever have is the one you take off your own device *before* you overwrite it.
The Adafruit UF2 bootloader can produce that: while a device sits in the
bootloader, its drive contains CURRENT.UF2, a read-back of what is installed.
Copy that file somewhere safe and you have a backup nobody has to give you.

But you cannot simply copy it back. The read-back differs from a flashable
image in two ways:

  1. It is tagged with a different UF2 family id (the bootloader stamps its own
     device id, 0x239a0029, on what it dumps; images it *accepts* are tagged
     0x621e937a for this nRF52833 board). A bootloader that checks the family
     id will reject its own dump.

  2. It spans the SoftDevice as well as the application -- it starts at 0x1000,
     while an application image starts at 0x27000. Writing the SoftDevice back
     is a heavier and riskier operation than restoring an app, and it is not
     needed: flashing an application never erased the SoftDevice in the first
     place, so the one on the device is still the one the backup was taken with.

This tool rewrites the family id and, by default, keeps only the application.
The result has exactly the same shape as the firmware images in firmware/, so
it flashes by the same drag-and-drop as everything else.

!! YOUR BACKUP CONTAINS SECRETS -- DO NOT SHARE CURRENT.UF2 !!
The read-back covers the whole flash, and that includes the settings partition,
where the Bluetooth stack keeps its bonds. Inspecting a real one shows entries
like "bt/keys/<the paired device's address>" alongside the long-term keys
themselves. Posting CURRENT.UF2 in a forum thread or a bug report hands over
your pairing keys and the addresses of devices you have paired with. Keep it
local. This tool excludes the settings partition by default, so its OUTPUT is
firmware only -- prefer sharing nothing, but if you must share something for
debugging, share the output, never the source.

USAGE
-----
    python uf2_rescue.py CURRENT.UF2 restore_stock.uf2
    python uf2_rescue.py CURRENT.UF2 restore_full.uf2 --with-softdevice

Then flash restore_stock.uf2 like any other image (flash.ps1 / flash.sh copy it
onto the bootloader drive).

VERIFY IT FIRST
---------------
Run it against a read-back taken from a device whose firmware you already have,
and diff the output against that known image. If they match byte for byte, the
round trip works on your hardware. Do that once, on a half (which is
recoverable), before you ever rely on it for the dongle (which is not).
"""
import argparse
import struct
import sys

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
FLAG_FAMILY_PRESENT = 0x00002000

# What this board's bootloader accepts, and what our own images are built with.
NRF52833_FAMILY = 0x621E937A
# Where the application starts on this board (below it: MBR + SoftDevice).
APP_START = 0x27000
# Where the code partition ends and the settings partition begins. Everything
# from here up is NVS: Bluetooth bonds, saved keymaps, backlight state. It is
# not firmware, it is per-device state, and it holds pairing keys -- so it is
# excluded from a restore image unless explicitly asked for.
STORAGE_START = 0x6C000


def parse_blocks(data):
    if len(data) % 512:
        sys.exit(f"not a UF2: size {len(data)} is not a multiple of 512")
    out = []
    for i in range(len(data) // 512):
        b = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, plen, blkno, nblk, famid = struct.unpack("<8I", b[:32])
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1:
            sys.exit(f"not a UF2: bad magic in block {i}")
        if struct.unpack("<I", b[-4:])[0] != UF2_MAGIC_END:
            sys.exit(f"not a UF2: bad end magic in block {i}")
        if plen > 476:
            sys.exit(f"block {i}: payload length {plen} out of range")
        out.append({"flags": flags, "addr": addr, "payload": b[32:32 + plen]})
    return out


def build(blocks, family):
    total = len(blocks)
    chunks = []
    for i, blk in enumerate(blocks):
        payload = blk["payload"]
        hdr = struct.pack(
            "<8I",
            UF2_MAGIC0,
            UF2_MAGIC1,
            blk["flags"] | FLAG_FAMILY_PRESENT,
            blk["addr"],
            len(payload),
            i,
            total,
            family,
        )
        body = payload.ljust(476, b"\x00")
        chunks.append(hdr + body + struct.pack("<I", UF2_MAGIC_END))
    return b"".join(chunks)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source", help="CURRENT.UF2 copied off the bootloader drive")
    ap.add_argument("dest", help="flashable image to write")
    ap.add_argument("--with-softdevice", action="store_true",
                    help="keep everything, including the SoftDevice below 0x27000 "
                         "(heavier and riskier; usually unnecessary)")
    ap.add_argument("--include-storage", action="store_true",
                    help="also keep the settings partition (Bluetooth bonds, saved "
                         "state). CONTAINS PAIRING KEYS -- never share the result")
    ap.add_argument("--family", default=hex(NRF52833_FAMILY),
                    help=f"UF2 family id to stamp (default {hex(NRF52833_FAMILY)})")
    ap.add_argument("--app-start", default=hex(APP_START),
                    help=f"application start address (default {hex(APP_START)})")
    ap.add_argument("--storage-start", default=hex(STORAGE_START),
                    help=f"settings partition start (default {hex(STORAGE_START)})")
    a = ap.parse_args()

    family = int(a.family, 0)
    app_start = int(a.app_start, 0)
    storage_start = int(a.storage_start, 0)

    blocks = parse_blocks(open(a.source, "rb").read())
    src_lo = min(b["addr"] for b in blocks)
    src_hi = max(b["addr"] for b in blocks) + 256
    src_fams = {b.get("famid") for b in blocks}
    print(f"read {len(blocks)} blocks covering 0x{src_lo:05x}..0x{src_hi:05x}")

    if not a.with_softdevice:
        kept = [b for b in blocks if b["addr"] >= app_start]
        dropped = len(blocks) - len(kept)
        if not kept:
            sys.exit(f"nothing at or above 0x{app_start:05x} -- wrong --app-start?")
        print(f"dropping {dropped} block(s) below 0x{app_start:05x} (SoftDevice region)")
    else:
        kept = blocks
        print("keeping the SoftDevice region as well (--with-softdevice)")

    if not a.include_storage:
        before = len(kept)
        kept = [b for b in kept if b["addr"] < storage_start]
        if before != len(kept):
            print(f"dropping {before - len(kept)} block(s) at or above "
                  f"0x{storage_start:05x} (settings partition: bonds and saved state)")
    else:
        print("WARNING: keeping the settings partition -- the output will contain "
              "Bluetooth pairing keys. Do not share it.")

    # Blank tail blocks carry no information and only slow the copy down.
    trimmed = [b for b in kept if b["payload"].strip(b"\xff")]
    if len(trimmed) != len(kept):
        print(f"dropping {len(kept) - len(trimmed)} erased (all-0xFF) block(s)")
    if not trimmed:
        sys.exit("every block was blank -- nothing to restore")

    out = build(trimmed, family)
    open(a.dest, "wb").write(out)
    lo = min(b["addr"] for b in trimmed)
    hi = max(b["addr"] for b in trimmed) + 256
    print(f"wrote {a.dest}: {len(trimmed)} blocks, 0x{lo:05x}..0x{hi:05x}, "
          f"family {family:#010x} ({len(out)} bytes)")
    print("Verify against a known-good image before trusting it (see the header).")


if __name__ == "__main__":
    main()
