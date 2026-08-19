<#
.SYNOPSIS
  Easy flasher for the NocFree & ZMK firmware. Pure PowerShell -- no Python,
  no installs. Works on any stock Windows.

.DESCRIPTION
  Run it with no arguments for a guided menu: it shows which of your devices
  are plugged in and walks you through flashing them in the right order.

  Or name a target directly to skip the menu.

  Either way it finds the device by USB VID/PID, drops it into the UF2
  bootloader with a 1200-baud touch (no buttons), copies the matching
  firmware\*.uf2 onto the bootloader drive, and confirms the reboot.

  Entering the bootloader erases nothing; if a copy does not happen, unplug/
  replug and the existing firmware boots again.

.EXAMPLE
  .\flash.ps1                # guided menu (start here)
  .\flash.ps1 left
  .\flash.ps1 right
  .\flash.ps1 dongle         # requires typing a confirmation -- see the warning

.NOTES
  THE DONGLE CAN BE BRICKED PERMANENTLY. It has no buttons and no reset
  pinhole; its only recovery is the software touch, so an interrupted or wrong
  flash can kill it for good. Flashing the dongle here requires typing a
  confirmation, and you should FIRST back up your own stock dongle firmware and
  read docs\DONGLE_SAFETY.md. macOS/Linux users: use flash.sh.
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
    left   = @{ Pid = '9129'; Uf2 = 'nocfree_left.uf2';           Label = 'Left half'  }
    right  = @{ Pid = '9229'; Uf2 = 'nocfree_right.uf2';          Label = 'Right half' }
    dongle = @{ Pid = '9029'; Uf2 = 'nocfree_dongle_BRIDGE.uf2';  Label = 'Dongle'     }
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

function Get-NocFreeBootloaderDrives {
    @(Get-DriveSet | Where-Object { Test-NocFreeBootloader $_ })
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

# What is plugged in right now? Used by the menu so you are not guessing.
function Show-Devices {
    $bootDrives = Get-NocFreeBootloaderDrives
    Write-Host ""
    Write-Host "  Connected devices" -ForegroundColor Cyan
    foreach ($name in 'right', 'left', 'dongle') {
        $t = $Targets[$name]
        $com = Find-Com $t.Pid
        if ($com) {
            Write-Host ("    {0,-11} running   ({1})" -f $t.Label, $com) -ForegroundColor Green
        }
        else {
            Write-Host ("    {0,-11} not detected" -f $t.Label) -ForegroundColor DarkGray
        }
    }
    if ($bootDrives.Count -gt 0) {
        Write-Host ("    Bootloader  drive {0}: waiting for firmware" -f ($bootDrives -join ':, ')) -ForegroundColor Yellow
    }
    Write-Host ""
}

# Returns $true on success, $false on failure -- never exits, so the menu can
# keep going after a failed attempt.
function Invoke-Flash($name) {
    $info = $Targets[$name]
    $uf2 = Join-Path $PSScriptRoot "firmware\$($info.Uf2)"
    if (-not (Test-Path $uf2)) {
        Write-Host "Firmware not found: $uf2" -ForegroundColor Red
        Write-Host "Build it, or use the firmware\ folder from the repo."
        return $false
    }

    if ($name -eq 'dongle') {
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
            return $false
        }
    }

    $com = Find-Com $info.Pid
    $drive = $null

    if (-not $com) {
        # Already sitting in the bootloader? That is the normal state after a
        # cancelled attempt, or after a trial build hands itself back -- there is
        # no COM port to touch then, but the drive is right there and ready.
        $mounted = Get-NocFreeBootloaderDrives
        if ($mounted.Count -eq 1) {
            $drive = $mounted[0]
            Write-Host "No $name COM port, but a NocFree bootloader drive is mounted on ${drive}: -- using it." -ForegroundColor Yellow
        }
        elseif ($mounted.Count -gt 1) {
            Write-Host "Several NocFree bootloader drives are mounted ($($mounted -join ', ')); unplug all but the $name." -ForegroundColor Red
            return $false
        }
        else {
            Write-Host "Could not find the $name on USB (VID_${VID}&PID_$($info.Pid))." -ForegroundColor Red
            Write-Host "Is it plugged in and running (not already in the bootloader)?"
            return $false
        }
    }

    if (-not $drive) {
        Write-Host "$name is on $com; entering bootloader ..."
        $before = Get-DriveSet
        try { Enter-Bootloader $com }
        catch {
            Write-Host "Could not open ${com}: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host "Close any serial monitor using the port and try again."
            return $false
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
            return $false
        }
    }

    if (-not (Test-NocFreeBootloader $drive)) {
        Write-Host "Drive ${drive}: is a UF2 bootloader but does NOT identify as a NocFree board." -ForegroundColor Red
        Write-Host "Refusing to copy -- flashing this firmware onto another board would break it."
        Write-Host "Unplug other UF2 devices and retry."
        return $false
    }

    Write-Host "Copying $($info.Uf2) -> ${drive}:\ ..."
    # The board reboots the instant the last block lands, so the volume can
    # vanish mid-write. Windows then reports the copy as failed even though the
    # flash succeeded -- and with $ErrorActionPreference='Stop' that would abort
    # with a scary error after a perfectly good flash. Swallow it here; the
    # post-copy check below is what actually decides success.
    try { Copy-Item $uf2 "${drive}:\" -ErrorAction Stop }
    catch { Write-Host "  (copy reported '$($_.Exception.Message.Trim())' -- expected if the board rebooted mid-write; verifying)" -ForegroundColor DarkGray }
    Start-Sleep -Seconds 5   # the board reboots itself once the UF2 lands

    if (Test-Path "${drive}:\INFO_UF2.TXT") {
        Write-Host "Still in the bootloader -- the copy may not have taken. Retry." -ForegroundColor Red
        return $false
    }
    Write-Host "Done: $name flashed and rebooted." -ForegroundColor Green
    return $true
}

function Show-Menu {
    Write-Host ""
    Write-Host "  NocFree & -- ZMK firmware flasher" -ForegroundColor Cyan
    Write-Host "  ---------------------------------"
    Show-Devices
    Write-Host "  What would you like to flash?"
    Write-Host "    1) Both halves      (right, then left -- the recommended order)"
    Write-Host "    2) Right half only"
    Write-Host "    3) Left half only"
    Write-Host "    4) Dongle           (brick risk -- read docs\DONGLE_SAFETY.md first)" -ForegroundColor Yellow
    Write-Host "    R) Re-scan devices"
    Write-Host "    Q) Quit             (nothing has been touched)"
    Write-Host ""
}

# --- direct mode: a target was named, just do it -----------------------------
if ($Target) {
    if (Invoke-Flash $Target) { exit 0 } else { exit 1 }
}

# --- guided mode -------------------------------------------------------------
Write-Host ""
Write-Host "Plug in the device(s) you want to flash. Entering the bootloader" -ForegroundColor DarkGray
Write-Host "erases nothing, and you can quit at any prompt." -ForegroundColor DarkGray

while ($true) {
    Show-Menu
    $choice = (Read-Host "  Choice").Trim().ToUpper()
    switch ($choice) {
        '1' {
            # Right first: the left half is the split central and re-pairs to
            # whatever the right is running, so flashing the right first avoids
            # a mismatched pair in between.
            if (Invoke-Flash 'right') {
                Write-Host "Right done. Give it a couple of seconds, then the left ..." -ForegroundColor DarkGray
                Start-Sleep -Seconds 3
                [void](Invoke-Flash 'left')
            }
            else {
                Write-Host "Stopping before the left half, since the right did not flash." -ForegroundColor Yellow
            }
        }
        '2' { [void](Invoke-Flash 'right') }
        '3' { [void](Invoke-Flash 'left') }
        '4' { [void](Invoke-Flash 'dongle') }
        'R' { }   # loop re-scans on redraw
        'Q' { Write-Host "Bye." -ForegroundColor Green; exit 0 }
        default { Write-Host "  Please choose 1, 2, 3, 4, R or Q." -ForegroundColor Yellow }
    }
}
