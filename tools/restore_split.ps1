# restore_split.ps1 -- one-shot split-link restore + verification round.
#
# Precondition: both halves on USB; the RIGHT half has just been physically
# replugged (unwedges its COM port). The LEFT half may already be sitting in
# its UF2 bootloader (drive mounted) from the probe/trial autodfu.
#
# What it does:
#   1. If a UF2 volume is already mounted -> that's the LEFT half; flash the
#      left TRIAL (bonds KEPT since the 08-11 bondfix era; self-return 300 s).
#      Otherwise touch the left's CDC port (VID_2886&PID_9129) at 1200 baud.
#   2. Touch the right's CDC port (VID_2886&PID_9229), wait for a NEW UF2
#      volume, flash the right TRIAL.
#   3. BLE scan (60 s). Trials keep bonds now, so the halves may re-link
#      before the scan even starts: 2 advertisers collapsing to 1 is success,
#      but so is seeing only 1 (or 0, once the dongle also links) the whole
#      time. The FAILURE signal is 2 advertisers that never collapse.
#
# Run from the repo root:  powershell -File tools\restore_split.ps1
#Requires -Version 5.1
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$PySys = "python"   # system python (bleak installed)
$PyVenv = "python"   # or the full path to a venv python, if you use one

function Get-HalfCom([string]$vidpid) {
    $p = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue |
         Where-Object { $_.InstanceId -match $vidpid } | Select-Object -First 1
    if (-not $p) { return $null }
    if ($p.FriendlyName -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

function Wait-NewUF2Volume([string[]]$exclude, [int]$seconds) {
    $t0 = Get-Date
    while ((Get-Date) - $t0 -lt (New-TimeSpan -Seconds $seconds)) {
        $vols = Get-Volume -ErrorAction SilentlyContinue |
                Where-Object { $_.FileSystemLabel -match 'NocFree|UF2' -and
                               $_.DriveLetter -and
                               ($exclude -notcontains $_.DriveLetter) }
        if ($vols) { return $vols[0].DriveLetter }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Flash-Uf2([string]$drive, [string]$uf2) {
    Copy-Item (Join-Path $Root $uf2) ("{0}:\{1}" -f $drive, $uf2)
    Write-Output ("[{0}] flashed {1} -> {2}:" -f (Get-Date -Format HH:mm:ss), $uf2, $drive)
}

# --- 1. left ---------------------------------------------------------------
$existing = @(Get-Volume -ErrorAction SilentlyContinue |
              Where-Object { $_.FileSystemLabel -match 'NocFree|UF2' -and $_.DriveLetter })
if ($existing.Count -eq 1) {
    Write-Output "left half already in bootloader on $($existing[0].DriveLetter):"
    Flash-Uf2 $existing[0].DriveLetter "nocfree_left_TRIAL.uf2"
} else {
    $com = Get-HalfCom "VID_2886&PID_9129"
    if (-not $com) { throw "left half CDC port (9129) not found" }
    Write-Output "touching left ($com) ..."
    & $PySys -u (Join-Path $Root "tools\dfu_touch.py") $com
    $d = Wait-NewUF2Volume @() 30
    if (-not $d) { throw "left UF2 drive never appeared" }
    Flash-Uf2 $d "nocfree_left_TRIAL.uf2"
}

# --- 2. right ---------------------------------------------------------------
$knownDrives = @((Get-Volume -ErrorAction SilentlyContinue |
    Where-Object { $_.FileSystemLabel -match 'NocFree|UF2' -and $_.DriveLetter } |
    ForEach-Object DriveLetter))
$comR = Get-HalfCom "VID_2886&PID_9229"
if (-not $comR) { throw "right half CDC port (9229) not found -- replug it" }
Write-Output "touching right ($comR) ..."
& $PySys -u (Join-Path $Root "tools\dfu_touch.py") $comR
$dR = Wait-NewUF2Volume $knownDrives 30
if (-not $dR) { throw "right UF2 drive never appeared" }
Flash-Uf2 $dR "nocfree_right_TRIAL.uf2"

# --- 3. verify: BLE scan ----------------------------------------------------
Write-Output "waiting 25 s for boot + split pairing, then scanning 60 s ..."
Start-Sleep -Seconds 25
$scan = @'
import asyncio
from bleak import BleakScanner

async def main():
    seen = {}
    def cb(dev, adv):
        if adv.local_name and "nocfree" in adv.local_name.lower():
            seen[dev.address] = (adv.local_name, asyncio.get_event_loop().time())
    s = BleakScanner(cb)
    await s.start()
    for i in range(12):
        await asyncio.sleep(5)
        print(f"t={(i+1)*5:3d}s  NocFree advertisers: {len(seen)}  {sorted(seen)}", flush=True)
    await s.stop()

asyncio.run(main())
'@
$scan | & $PySys -u -
Write-Output ""
Write-Output "Reading the scan: 2 -> 1 advertisers means the split link formed."
Write-Output "Bonds are KEPT in trials now, so a constant 1 (or 0) is also fine --"
Write-Output "the halves may have re-linked before the scan started. Only a"
Write-Output "constant 2 (never collapsing) is a failure."
Write-Output "If it stays at 2: re-run this script. Next step after success:"
Write-Output "flash nocfree_left.uf2 + nocfree_right.uf2 (keepers) the same way."
