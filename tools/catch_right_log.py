"""Wait for the right half's CDC port, open it IMMEDIATELY at 115200, and record.

Companion to catch_right.py, but instead of DFU-touching the port it captures
the diagnostic console (diag.conf build: CONFIG_LOG_MODE_IMMEDIATE on the CDC
console). The point is to catch the kscan/I2C log lines emitted between USB
enumeration and the suspected hang.

115200 is nowhere near the 1200-baud DFU magic, so opening the port is safe.
Output goes to stdout and to right_diag_capture.log next to the repo root.
"""
import subprocess, sys, time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial missing: pip install pyserial")

TARGET = "PID_9229"
DEADLINE = 180          # seconds to wait for the port to appear
CAPTURE_FOR = 60        # seconds to keep reading once open
LOGFILE = "right_diag_capture.log"

def right_port():
    ps = ("Get-PnpDevice -PresentOnly -EA SilentlyContinue | "
          "Where-Object { $_.Class -eq 'Ports' -and $_.InstanceId -match '%s' } | "
          "ForEach-Object { $_.FriendlyName }" % TARGET)
    try:
        out = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return None
    for tok in out.replace("(", " ").replace(")", " ").split():
        if tok.upper().startswith("COM"):
            return tok.upper()
    return None

print("Watching for the RIGHT half's CDC port (plug it in / it reboots after flash)...",
      flush=True)
t0 = time.time()
port = None
while time.time() - t0 < DEADLINE:
    p = right_port()
    if p:
        port = p
        break
    time.sleep(0.2)
if not port:
    sys.exit("timed out waiting for the port")

print(f"APPEARED: {port} -- opening at 115200", flush=True)
buf = b""
with open(LOGFILE, "ab") as f:
    f.write(b"\n===== capture %s =====\n" % time.strftime("%Y-%m-%d %H:%M:%S").encode())
    end = time.time() + CAPTURE_FOR
    ser = None
    while time.time() < end:
        if ser is None:
            try:
                ser = serial.Serial(port, 115200, timeout=0.2)
                ser.dtr = True   # Zephyr CDC console gates output on DTR
                print("port open, DTR asserted, reading...", flush=True)
            except Exception as e:
                # enumeration race: the port node exists before the driver is ready
                time.sleep(0.1)
                continue
        try:
            chunk = ser.read(4096)
        except Exception as e:
            print(f"\n-- port died ({e}); device reset or hang took USB down --",
                  flush=True)
            f.write(b"\n-- PORT DIED: %s --\n" % str(e).encode())
            ser = None
            # keep looping: if it re-enumerates we reattach and keep capturing
            time.sleep(0.3)
            new = right_port()
            if new:
                port = new
            continue
        if chunk:
            f.write(chunk)
            f.flush()
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
print("\ncapture window over; saved to", LOGFILE, flush=True)
