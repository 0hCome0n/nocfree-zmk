<#
.SYNOPSIS
  Easy flasher for the NocFree & ZMK firmware. Pure PowerShell -- no Python,
  no installs. Works on any stock Windows.

.DESCRIPTION
  Finds a device by USB VID/PID, drops it into the UF2 bootloader with a
  1200-baud touch (no buttons), copies the matching firmware\*.uf2 onto the
  bootloader drive, and confirms the reboot.

  Entering the bootloader erases nothing; if a copy does not happen, unplug/
  replug and the existing firmware boots again.

.EXAMPLE
  .\flash.ps1 left
  .\flash.ps1 right
  .\flash.ps1 dongle        # prompts for confirmation -- see the warning below

.NOTES
  THE DONGLE CAN BE BRICKED PERMANENTLY. It has no buttons and no reset
  pinhole; its only recovery is the software touch, so an interrupted or wrong
  flash can kill it for good. Flashing the dongle here requires typing a
  confirmation, and you should FIRST back up your own stock dongle firmware and
  read docs\DONGLE_SAFETY.md. macOS/Linux users: see the README (manual method).
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('left', 'right', 'dongle')]
    [string]$Target
)

$ErrorActionPreference = 'Stop'
$VID = '2886'
$Targets = @{
    left   = @{ Pid = '9129'; Uf2 = 'nocfree_left.uf2' }
    right  = @{ Pid = '9229'; Uf2 = 'nocfree_right.uf2' }
    dongle = @{ Pid = '9029'; Uf2 = 'nocfree_dongle_BRIDGE.uf2' }
}

function Find-Com($devicePid) {
    $m = Get-CimInstance Win32_PnPEntity | Where-Object {
        $_.DeviceID -match "VID_${VID}&PID_${devicePid}&MI_00"
    } | ForEach-Object {
        if ($_.Name -match '\((COM\d+)\)') { $Matches[1] }
    }
    if ($m) { return @($m)[0] } else { return $null }
}

function Get-DriveSet { (Get-PSDrive -PSProvider FileSystem).Name }

# A UF2 bootloader drive identifies its board in INFO_UF2.TXT. Ours reports
# "NocFree &" as both Model and Board-ID. Checking it stops a stray copy onto
# some other UF2 board (CircuitPython, Arduino, another keyboard) that happens
# to be mounted -- that board would be bricked by our firmware, not ours.
function Test-NocFreeBootloader($driveLetter) {
    $info = "${driveLetter}:\INFO_UF2.TXT"
    if (-not (Test-Path $info)) { return $false }
    try { return ((Get-Content $info -Raw -ErrorAction Stop) -match 'NocFree') }
    catch { return $false }
}

function Enter-Bootloader($com) {
    # 1200-baud open with a DTR drop is the reset-to-bootloader trigger.
    if (-not ('System.IO.Ports.SerialPort' -as [type])) {
        throw "System.IO.Ports.SerialPort is unavailable in this host. Run this script under Windows PowerShell 5.1 (powershell.exe), which ships it."
    }
    $p = New-Object System.IO.Ports.SerialPort($com, 1200)
    try {
        $p.Open()
        $p.DtrEnable = $true
        Start-Sleep -Milliseconds 60
        $p.DtrEnable = $false     # the drop
        Start-Sleep -Milliseconds 60
    }
    finally {
        if ($p.IsOpen) { $p.Close() }
        $p.Dispose()
    }
}

if (-not $Target) {
    Get-Help $PSCommandPath -Detailed
    exit 1
}

$info = $Targets[$Target]
$uf2 = Join-Path $PSScriptRoot "firmware\$($info.Uf2)"
if (-not (Test-Path $uf2)) {
    Write-Host "Firmware not found: $uf2" -ForegroundColor Red
    Write-Host "Build it, or use the firmware\ folder from the repo."
    exit 1
}

if ($Target -eq 'dongle') {
    Write-Host ""
    Write-Host "  !! DONGLE FLASH -- READ THIS !!" -ForegroundColor Yellow
    Write-Host "  The dongle has no buttons and no reset pinhole. A bad or"
    Write-Host "  interrupted flash can brick it PERMANENTLY. Before continuing you"
    Write-Host "  should have BACKED UP your own stock dongle firmware and read"
    Write-Host "  docs\DONGLE_SAFETY.md. This is entirely at your own risk."
    Write-Host ""
    $reply = Read-Host "  Type  I UNDERSTAND  to proceed (anything else cancels)"
    if ($reply -ne 'I UNDERSTAND') {
        Write-Host "Cancelled -- nothing was touched." -ForegroundColor Green
        exit 0
    }
}

$com = Find-Com $info.Pid
$drive = $null

if (-not $com) {
    # Already sitting in the bootloader? That is the normal state after a
    # cancelled attempt, or after a trial build hands itself back -- there is no
    # COM port to touch then, but the drive is right there and ready.
    $mounted = @(Get-DriveSet | Where-Object { Test-NocFreeBootloader $_ })
    if ($mounted.Count -eq 1) {
        $drive = $mounted[0]
        Write-Host "No $Target COM port, but a NocFree bootloader drive is mounted on ${drive}: -- using it." -ForegroundColor Yellow
    }
    elseif ($mounted.Count -gt 1) {
        Write-Host "Several NocFree bootloader drives are mounted ($($mounted -join ', ')); unplug all but the $Target." -ForegroundColor Red
        exit 1
    }
    else {
        Write-Host "Could not find the $Target on USB (VID_${VID}&PID_$($info.Pid))." -ForegroundColor Red
        Write-Host "Is it plugged in and running (not already in the bootloader)?"
        exit 1
    }
}

if (-not $drive) {
    Write-Host "$Target is on $com; entering bootloader ..."
    $before = Get-DriveSet
    try { Enter-Bootloader $com }
    catch {
        Write-Host "Could not open ${com}: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "Close any serial monitor using the port and try again."
        exit 1
    }

    Write-Host "Waiting for the bootloader drive ..."
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        foreach ($d in (Get-DriveSet | Where-Object { $_ -notin $before })) {
            if (Test-Path "${d}:\INFO_UF2.TXT") { $drive = $d; break }
        }
        if ($drive) { break }
        Start-Sleep -Milliseconds 400
    }
    if (-not $drive) {
        Write-Host "No bootloader drive appeared. Unplug/replug and retry;" -ForegroundColor Red
        Write-Host "the existing firmware is untouched."
        exit 1
    }
}

if (-not (Test-NocFreeBootloader $drive)) {
    Write-Host "Drive ${drive}: is a UF2 bootloader but does NOT identify as a NocFree board." -ForegroundColor Red
    Write-Host "Refusing to copy -- flashing this firmware onto another board would break it."
    Write-Host "Unplug other UF2 devices and retry."
    exit 1
}

Write-Host "Copying $($info.Uf2) -> ${drive}:\ ..."
# The board reboots the instant the last block lands, so the volume can vanish
# mid-write. Windows then reports the copy as failed even though the flash
# succeeded -- and with $ErrorActionPreference='Stop' that would abort the
# script with a scary error after a perfectly good flash. Swallow it here; the
# post-copy check below is what actually decides success.
try { Copy-Item $uf2 "${drive}:\" -ErrorAction Stop }
catch { Write-Host "  (copy reported '$($_.Exception.Message.Trim())' -- expected if the board rebooted mid-write; verifying)" -ForegroundColor DarkGray }
Start-Sleep -Seconds 5   # the board reboots itself once the UF2 lands

if (Test-Path "${drive}:\INFO_UF2.TXT") {
    Write-Host "Still in the bootloader -- the copy may not have taken. Retry." -ForegroundColor Red
    exit 1
}
Write-Host "Done: $Target flashed and rebooted." -ForegroundColor Green
