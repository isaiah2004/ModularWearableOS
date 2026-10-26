# Archived core/apps simulator; use nos_build.ps1 for current NOS.
param([string]$OutputName = 'sim.exe', [string]$Python = 'python', [string]$Compiler)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Write-Warning 'Building archived core/apps UI. Current watch runs NOS; use sim/nos_build.ps1.'
$arguments = @((Join-Path $root 'tools/build_sim.py'), '--legacy', '--out', $OutputName)
if ($Compiler) { $arguments += @('--compiler', $Compiler) }
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw "Legacy simulator build failed (exit $LASTEXITCODE)" }
