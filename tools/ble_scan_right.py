"""Scan BLE advertisements for the right half's radio.

If the bricked right half's boot survives past BT init before the fatal halt,
its ZMK split peripheral is advertising for its bonded central. Advertised
name would be the ZMK keyboard name ("NocFree &"). Scan and dump everything
so a name mismatch doesn't hide it.
"""
import asyncio, sys
from bleak import BleakScanner

async def main():
    print("scanning 30s...", flush=True)
    seen = {}

    def cb(device, adv):
        key = device.address
        entry = (adv.local_name, adv.rssi, tuple(sorted(adv.service_uuids or [])))
        if seen.get(key) != entry:
            seen[key] = entry
            print(f"  {device.address}  rssi={adv.rssi:>4}  name={adv.local_name!r}  "
                  f"uuids={list(adv.service_uuids or [])[:4]}", flush=True)

    scanner = BleakScanner(cb)
    await scanner.start()
    await asyncio.sleep(30)
    await scanner.stop()
    print(f"\n{len(seen)} distinct advertisers.", flush=True)
    hits = [a for a, (n, _, _) in seen.items() if n and "nocfree" in n.lower()]
    if hits:
        print("NOCFREE ADVERTISER(S) FOUND: " + ", ".join(hits), flush=True)
    else:
        print("no NocFree-named advertiser (bonded-peripheral adv may be nameless "
              "-- check UUID 0000180f / HID 00001812 entries above too)", flush=True)

asyncio.run(main())
