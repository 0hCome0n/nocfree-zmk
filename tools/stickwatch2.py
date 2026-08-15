"""Stuck-key diagnostic logger, v2 -- built so it cannot hold your keyboard hostage.

WHY v1 WAS BAD: it did a file write inside the hook callback on every keystroke.
A WH_KEYBOARD_LL callback must return within LowLevelHooksTimeout (~300ms) or
Windows starts delaying and discarding input system-wide. Per-keystroke I/O from a
background process is more than enough to trip that. And the only documented way to
stop it was Ctrl-C -- a keyboard action, in a tool that can break the keyboard.

WHAT CHANGED:
  * the hook callback now does ONE thing: append a tuple to a list. No I/O, no
    formatting, no locks. Microseconds, so the timeout can't trip.
  * a watchdog thread ends the run three independent ways, none needing the keyboard:
      - a hard wall-clock limit (default 120s)
      - the appearance of the stop file (STOP-STICKWATCH next to this script);
        create it from any shell, or another agent can `touch` it
      - Ctrl-C, still there, but no longer the only exit
  * results are written once, at exit.

Usage:  python stickwatch2.py [seconds]
Stop:   wait for the timer, or:  echo x > STOP-STICKWATCH
"""
import ctypes
import ctypes.wintypes as w
import os
import sys
import threading
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WH_KEYBOARD_LL = 13
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_SYSKEYDOWN, WM_SYSKEYUP = 0x0104, 0x0105
WM_QUIT = 0x0012

HERE = os.path.dirname(os.path.abspath(__file__))
STOPFILE = os.path.join(HERE, "STOP-STICKWATCH")
OUT = os.path.join(HERE, "stickwatch.log")
LIMIT = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("vkCode", w.DWORD), ("scanCode", w.DWORD), ("flags", w.DWORD),
                ("time", w.DWORD), ("dwExtraInfo", ctypes.POINTER(w.ULONG))]


LRESULT = ctypes.c_ssize_t
HOOKPROC = ctypes.WINFUNCTYPE(LRESULT, ctypes.c_int, w.WPARAM, w.LPARAM)

# Explicit prototypes are REQUIRED on 64-bit. Without them ctypes assumes C int
# (32-bit) for every argument and return, so passing lParam -- a 64-bit pointer --
# raises "OverflowError: int too long to convert" on the first keystroke, and the
# run dies before writing anything.
user32.SetWindowsHookExW.restype = w.HHOOK
user32.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, w.HINSTANCE, w.DWORD]
user32.CallNextHookEx.restype = LRESULT
user32.CallNextHookEx.argtypes = [w.HHOOK, ctypes.c_int, w.WPARAM, w.LPARAM]
user32.UnhookWindowsHookEx.restype = w.BOOL
user32.UnhookWindowsHookEx.argtypes = [w.HHOOK]
user32.GetMessageW.restype = ctypes.c_int
user32.GetMessageW.argtypes = [ctypes.POINTER(w.MSG), w.HWND,
                               ctypes.c_uint, ctypes.c_uint]
user32.PostThreadMessageW.restype = w.BOOL
user32.PostThreadMessageW.argtypes = [w.DWORD, ctypes.c_uint, w.WPARAM, w.LPARAM]
kernel32.GetCurrentThreadId.restype = w.DWORD
kernel32.GetCurrentThreadId.argtypes = []

events = []          # (t, is_up, vk, sc, injected) -- appended to, nothing else
T0 = time.perf_counter()
main_tid = kernel32.GetCurrentThreadId()


errors = []
stopping = threading.Event()
ECHO = "--no-echo" not in sys.argv


def handler(nCode, wParam, lParam):
    # MUST be fast, and must NEVER raise: an exception escaping a low-level hook
    # leaves Windows waiting on us and stalls input for the whole system.
    try:
        if nCode == 0:
            kb = ctypes.cast(lParam, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
            events.append((time.perf_counter() - T0,
                           wParam in (WM_KEYUP, WM_SYSKEYUP),
                           kb.vkCode, kb.scanCode, bool(kb.flags & 0x10)))
    except BaseException as e:                     # noqa: BLE001 - deliberate
        if len(errors) < 5:
            errors.append(repr(e))
    return user32.CallNextHookEx(None, nCode, wParam, lParam)


def echo_loop():
    """Live echo of what the host actually received.

    Runs on its OWN thread and only ever reads the events list. The hook callback
    must never do I/O -- console writes from inside a WH_KEYBOARD_LL callback are
    precisely what stalls system-wide input.
    """
    seen = 0
    col = 0
    while not stopping.is_set():
        time.sleep(0.05)
        while seen < len(events):
            _, is_up, vk, sc, _ = events[seen]
            seen += 1
            if is_up:
                continue
            if vk == 0x08:                        # backspace
                sys.stdout.write("\b \b")
                col = max(0, col - 1)
            elif vk == 0x0D:                      # enter
                sys.stdout.write("\n")
                col = 0
            elif vk == 0x20:
                sys.stdout.write(" ")
                col += 1
            elif 0x30 <= vk <= 0x5A:
                sys.stdout.write(chr(vk).lower())
                col += 1
            else:
                continue
            if col >= 100:                        # keep it readable
                sys.stdout.write("\n")
                col = 0
        sys.stdout.flush()


def watchdog():
    """Ends the run without needing any keyboard input."""
    deadline = time.perf_counter() + LIMIT
    while True:
        time.sleep(0.25)
        if os.path.exists(STOPFILE):
            print("\n[stop file seen -- ending]", flush=True)
            break
        if time.perf_counter() > deadline:
            print("\n[time limit reached -- ending]", flush=True)
            break
    stopping.set()
    user32.PostThreadMessageW(main_tid, WM_QUIT, 0, 0)


def vkname(vk):
    return chr(vk) if 0x30 <= vk <= 0x5A else f"vk{vk:#04x}"


def report():
    down, lines, phantom, longheld = {}, [], 0, 0
    for t, is_up, vk, sc, inj in events:
        if not is_up:
            if vk in down:
                lines.append(f"{t:9.3f}  rept  {vkname(vk):>6} sc={sc:#04x}")
            else:
                down[vk] = t
                lines.append(f"{t:9.3f}  DOWN  {vkname(vk):>6} sc={sc:#04x}  "
                             f"held_now={len(down)}")
        else:
            if vk in down:
                lines.append(f"{t:9.3f}  UP    {vkname(vk):>6} sc={sc:#04x}  "
                             f"held={t - down[vk]:.3f}s")
                del down[vk]
            else:
                phantom += 1
                lines.append(f"{t:9.3f}  *** PHANTOM RELEASE *** {vkname(vk)} "
                             f"sc={sc:#04x}{' injected' if inj else ''} "
                             f"(host had: {[vkname(k) for k in down]})")
    for vk, t0 in down.items():
        longheld += 1
        lines.append(f"{'':9}  *** NEVER RELEASED *** {vkname(vk)} "
                     f"down at {t0:.3f} and still down at exit")

    with open(OUT, "w") as f:
        f.write(f"# stickwatch v2 -- {len(events)} raw events over {LIMIT:g}s limit\n")
        f.write(f"# phantom releases: {phantom}   never released: {longheld}\n\n")
        f.write("\n".join(lines) + "\n")
    print(f"\n{len(events)} events -> {OUT}")
    print(f"phantom releases: {phantom}   keys never released: {longheld}")
    if errors:
        print(f"callback errors (first {len(errors)}): {errors}")


def main():
    if os.path.exists(STOPFILE):
        os.remove(STOPFILE)
    print(f"stickwatch v2: recording for up to {LIMIT:g}s.")
    print(f"stop early with:  echo x > \"{STOPFILE}\"")
    if ECHO:
        print("ECHO IS ON: typed text is shown on screen (--no-echo to disable).")
        print("Don't type passwords or anything private during a capture.\n")
    else:
        print("scancodes only -- no text is shown.\n")

    cb = HOOKPROC(handler)
    hook = user32.SetWindowsHookExW(WH_KEYBOARD_LL, cb, None, 0)
    if not hook:
        print(f"SetWindowsHookExW failed: {ctypes.get_last_error()}")
        return 1

    threading.Thread(target=watchdog, daemon=True).start()
    if ECHO:
        print("--- live echo (what the HOST received) "
              "-------------------------------")
        threading.Thread(target=echo_loop, daemon=True).start()
    msg = w.MSG()
    try:
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
    except KeyboardInterrupt:
        pass
    finally:
        user32.UnhookWindowsHookEx(hook)      # always released
        if os.path.exists(STOPFILE):
            os.remove(STOPFILE)
        report()
    return 0


if __name__ == "__main__":
    sys.exit(main())
