# Purge stale Windows BT bond state for ONE device (your left half) only.
# Run from an ELEVATED PowerShell, passing the keyboard's BT MAC with no
# separators (lower case), e.g.:
#   powershell -ExecutionPolicy Bypass -File .\tools\bt_purge_nocfree.ps1 aabbccddeeff
# Find it in Device Manager -> Bluetooth -> your keyboard -> Details ->
# "Bluetooth device address", or in the BTHPORT\Parameters\Devices key below.
# The BTHPORT Keys/Devices hives are ACL'd to SYSTEM, so this creates a
# one-shot scheduled task running as SYSTEM to do the surgery, then restarts
# bthserv. Log: .\bt_purge_log.txt

param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{12}$')]
    [string]$Mac
)

$ErrorActionPreference = 'Stop'
$mac = $Mac.ToLower()
$log = Join-Path (Get-Location) 'bt_purge_log.txt'
$cmdFile = "$env:TEMP\bt_purge_nocfree.cmd"

@"
@echo off
echo ===== BEFORE ===== > "$log"
reg query "HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Keys" /s >> "$log" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices\$mac" /s >> "$log" 2>&1
echo ===== DELETING ===== >> "$log"
for /f "delims=" %%K in ('reg query "HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Keys"') do (
  reg delete "%%K" /v $mac /f >> "$log" 2>&1
  reg delete "%%K\$mac" /f >> "$log" 2>&1
)
reg delete "HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices\$mac" /f >> "$log" 2>&1
echo ===== AFTER ===== >> "$log"
reg query "HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Keys" /s >> "$log" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices\$mac" /s >> "$log" 2>&1
"@ | Set-Content -Path $cmdFile -Encoding ASCII

schtasks /create /tn btpurge_nocfree /tr "cmd /c `"$cmdFile`"" /sc once /st 23:59 /ru SYSTEM /f | Out-Null
schtasks /run /tn btpurge_nocfree | Out-Null
Start-Sleep -Seconds 5
schtasks /delete /tn btpurge_nocfree /f | Out-Null

Write-Host "Registry surgery done. Restarting Bluetooth service (audio will blip)..."
Restart-Service bthserv -Force
Write-Host "Done. Log at $log - now retry pairing on an empty profile."
