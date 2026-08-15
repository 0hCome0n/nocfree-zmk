"""Wait for the right half's CDC port to appear, then IMMEDIATELY 1200-baud touch it.

Why: if the right half hangs in a blocking I2C call inside the kscan work item,
USB CDC may enumerate a moment BEFORE the hang. That leaves a short window in
which the port is still responsive. Racing it by hand is unreliable; this polls
hard and fires the instant the port shows up.
"""
import subprocess, sys, time
TARGET = "PID_9229"          # ZMK right half
DEADLINE = 1800

def right_port():
    ps = ("Get-PnpDevice -PresentOnly -EA SilentlyContinue | "
          "Where-Object { $_.Class -eq 'Ports' -and $_.InstanceId -match '%s' } | "
          "ForEach-Object { $_.FriendlyName }" % TARGET)
    try:
        out = subprocess.run(["powershell.exe","-NoProfile","-Command",ps],
                             capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return None
    for tok in out.replace("(", " ").replace(")", " ").split():
        if tok.upper().startswith("COM"):
            return tok.upper()
    return None

print("Unplug the RIGHT half, wait 2s, then plug it back in.")
print("Watching for its port ...", flush=True)
seen = right_port()
print(f"  (currently: {seen})", flush=True)
t0 = time.time()
while time.time() - t0 < DEADLINE:
    p = right_port()
    if p and p != seen:
        print(f"  APPEARED: {p} -- touching immediately", flush=True)
        r = subprocess.run([sys.executable, "-u", "tools/dfu_touch.py", p, "--watch"],
                           capture_output=True, text=True, timeout=40)
        print(r.stdout or "(no output)", flush=True)
        print(r.stderr or "", flush=True)
        sys.exit(0)
    if p is None and seen is not None:
        print("  port went away (unplugged) -- waiting for replug", flush=True)
        seen = None
    time.sleep(0.2)
print("timed out")
