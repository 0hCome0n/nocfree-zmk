import subprocess, time, sys
print("watching 30 min for a UF2 bootloader drive or DFU port...", flush=True)
t0 = time.time()
while time.time() - t0 < 1800:
    out = subprocess.run(["powershell.exe","-NoProfile","-Command",
        "Get-Volume -EA SilentlyContinue | Where-Object FileSystemLabel -match 'NocFree|UF2' | ForEach-Object { $_.DriveLetter }; "
        "Get-PnpDevice -PresentOnly -EA SilentlyContinue | Where-Object { $_.InstanceId -match '239A&PID_002A|239A&PID_0029' } | ForEach-Object { 'DFU:' + $_.FriendlyName }"],
        capture_output=True, text=True, timeout=8).stdout.strip()
    if out:
        print(f"[{time.strftime('%H:%M:%S')}] FOUND: {out}", flush=True)
        sys.exit(0)
    time.sleep(0.4)
print("timed out", flush=True)
