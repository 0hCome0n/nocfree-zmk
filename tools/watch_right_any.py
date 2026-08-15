"""Broad watcher: report ANY right-half-shaped USB arrival for 10 minutes.

Watches for: 2886:9229 (ZMK right), 239A:002A/0029 (bootloader), 239A:80D8
(stock right). If the ZMK CDC appears, immediately opens it at 115200 and
captures the console to right_diag_capture.log (same as catch_right_log.py).
Everything else is just reported with a timestamp.
"""
import subprocess, sys, time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: pip install pyserial")

DEADLINE = 600
LOGFILE = "right_diag_capture.log"
PATTERNS = ["PID_9229", "PID_002A", "PID_0029", "PID_80D8"]

def snapshot():
    ps = ("Get-PnpDevice -PresentOnly -EA SilentlyContinue | "
          "Where-Object { $_.InstanceId -match '9229|002A|0029|80D8' } | "
          "ForEach-Object { $_.Class + '|' + $_.FriendlyName + '|' + $_.InstanceId }")
    try:
        out = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return set()
    return set(l.strip() for l in out.splitlines() if l.strip())

def com_of(line):
    for tok in line.replace("(", " ").replace(")", " ").split():
        if tok.upper().startswith("COM"):
            return tok.upper()
    return None

def capture(port):
    print(f"[{time.strftime('%H:%M:%S')}] ZMK right CDC {port} -- capturing 60s", flush=True)
    end = time.time() + 60
    ser = None
    with open(LOGFILE, "ab") as f:
        f.write(b"\n===== capture %s =====\n" % time.strftime("%H:%M:%S").encode())
        while time.time() < end:
            if ser is None:
                try:
                    ser = serial.Serial(port, 115200, timeout=0.2)
                    ser.dtr = True
                except Exception:
                    time.sleep(0.1)
                    continue
            try:
                chunk = ser.read(4096)
            except Exception as e:
                print(f"-- port died: {e}", flush=True)
                f.write(b"-- PORT DIED --\n")
                return
            if chunk:
                f.write(chunk); f.flush()
                sys.stdout.write(chunk.decode("utf-8", "replace")); sys.stdout.flush()

print("Watching (10 min) for any right-half USB identity...", flush=True)
seen = snapshot()
for s in seen:
    print(f"  baseline: {s}", flush=True)
t0 = time.time()
while time.time() - t0 < DEADLINE:
    now = snapshot()
    for new in now - seen:
        print(f"[{time.strftime('%H:%M:%S')}] APPEARED: {new}", flush=True)
        if "9229" in new and "|USB Serial" in new or ("9229" in new and com_of(new)):
            p = com_of(new)
            if p:
                capture(p)
    for gone in seen - now:
        print(f"[{time.strftime('%H:%M:%S')}] GONE: {gone}", flush=True)
    seen = now
    time.sleep(0.25)
print("watch window over", flush=True)
