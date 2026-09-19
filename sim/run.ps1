# Restart the simulator: kill any running instance, rebuild, relaunch.
# Usage: .\sim\run.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

try { Stop-Process -Name sim -Force -ErrorAction Stop } catch {}
& "$PSScriptRoot\build.ps1"
Start-Process "$root\build\sim.exe" -WorkingDirectory "$root\build"
Write-Host "simulator restarted"
