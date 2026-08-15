"""Put a NocFree device into its UF2 bootloader over USB -- no hotkey, no buttons.

The stock firmware already listens for this: a CDC line-state handler watches for a
DTR drop while the line coding is 1200 baud, then resets into the bootloader. So the
"app that forces DFU over USB" is just: open the port at 1200 baud, drop DTR, close.
That is the whole mechanism.

Pure ctypes -- pyserial is not installed and is not needed.

    python dfu_touch.py                 # list candidate ports, touch nothing
    python dfu_touch.py COM21           # trigger DFU on COM21
    python dfu_touch.py COM21 --watch   # trigger, then watch for the bootloader drive

Recovery note: this is reversible. If a device ends up in the bootloader and you do not
flash it, just unplug/replug (or wait out the bootloader timeout) and it boots the
existing firmware again. Nothing is erased by entering DFU.
"""
import ctypes
import ctypes.wintypes as w
import string
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE = ctypes.c_void_p(-1).value
CLRDTR, SETDTR = 6, 5


class DCB(ctypes.Structure):
    _fields_ = [("DCBlength", w.DWORD), ("BaudRate", w.DWORD), ("flags", w.DWORD),
                ("wReserved", w.WORD), ("XonLim", w.WORD), ("XoffLim", w.WORD),
                ("ByteSize", ctypes.c_byte), ("Parity", ctypes.c_byte),
                ("StopBits", ctypes.c_byte), ("XonChar", ctypes.c_char),
                ("XoffChar", ctypes.c_char), ("ErrorChar", ctypes.c_char),
                ("EofChar", ctypes.c_char), ("EvtChar", ctypes.c_char),
                ("wReserved1", w.WORD)]


# Explicit prototypes are mandatory on 64-bit: without them ctypes assumes C int and
# TRUNCATES the HANDLE returned by CreateFileW, so every later call gets a bad handle.
# (Same bug class that broke the keyboard hook earlier -- declare your signatures.)
k32.CreateFileW.restype = w.HANDLE
k32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                            w.DWORD, w.DWORD, w.HANDLE]
k32.GetCommState.restype = w.BOOL
k32.GetCommState.argtypes = [w.HANDLE, ctypes.POINTER(DCB)]
k32.SetCommState.restype = w.BOOL
k32.SetCommState.argtypes = [w.HANDLE, ctypes.POINTER(DCB)]
k32.EscapeCommFunction.restype = w.BOOL
k32.EscapeCommFunction.argtypes = [w.HANDLE, w.DWORD]
k32.CloseHandle.restype = w.BOOL
k32.CloseHandle.argtypes = [w.HANDLE]
k32.GetLogicalDrives.restype = w.DWORD
k32.GetLogicalDrives.argtypes = []


def list_ports():
    """Enumerate COM ports via the registry (no pyserial)."""
    import winreg
    out = []
    try:
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
    except OSError:
        return out
    i = 0
    while True:
        try:
            name, val, _ = winreg.EnumValue(key, i)
        except OSError:
            break
        out.append((val, name))
        i += 1
    return out


def touch(port, baud=1200):
    path = f"\\\\.\\{port}"
    h = k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, None,
                        OPEN_EXISTING, 0, None)
    if h is None or h == INVALID_HANDLE:
        err = ctypes.get_last_error()
        print(f"could not open {port} (error {err})"
              f"{' -- port busy? close any terminal using it' if err == 5 else ''}")
        return False
    try:
        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not k32.GetCommState(h, ctypes.byref(dcb)):
            print("GetCommState failed")
            return False
        dcb.BaudRate = baud
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        if not k32.SetCommState(h, ctypes.byref(dcb)):
            print(f"SetCommState failed (error {ctypes.get_last_error()})")
            return False
        print(f"{port}: line coding set to {baud} baud")
        k32.EscapeCommFunction(h, SETDTR)
        time.sleep(0.05)
        k32.EscapeCommFunction(h, CLRDTR)   # the DTR drop is the trigger
        time.sleep(0.05)
    finally:
        k32.CloseHandle(h)
    print(f"{port}: DTR dropped and port closed -- device should reset to bootloader")
    return True


def drives():
    mask = k32.GetLogicalDrives()
    return {string.ascii_uppercase[i] for i in range(26) if mask & (1 << i)}


def watch(before, seconds=12):
    print(f"watching for a new drive for {seconds}s ...")
    end = time.time() + seconds
    while time.time() < end:
        now = drives()
        new = now - before
        if new:
            for d in sorted(new):
                info = ""
                try:
                    with open(f"{d}:\\INFO_UF2.TXT") as f:
                        info = " | " + f.readline().strip()
                except OSError:
                    pass
                print(f"  NEW DRIVE {d}:{info}")
            return True
        time.sleep(0.4)
    print("  no new drive appeared")
    return False


if __name__ == "__main__":
    ports = list_ports()
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print("COM ports currently present:")
        for dev, src in ports:
            print(f"  {dev}   ({src})")
        print("\nRe-run with the port name, e.g.:  python dfu_touch.py COM21")
        print("Add --watch to also detect the bootloader drive appearing.")
        sys.exit(0)
    port = args[0]
    before = drives()
    if touch(port) and "--watch" in sys.argv:
        watch(before)
