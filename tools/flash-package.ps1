param([Parameter(Mandatory=$true)][string]$Port, [string]$Python='python', [switch]$DryRun)
$ErrorActionPreference = 'Stop'
# Distributed as flash.ps1. Uses the same checked host updater as tools/build.ps1.
$arguments = @((Join-Path $PSScriptRoot 'tooling/flash_watch.py'), '--package', $PSScriptRoot, '--port', $Port)
if ($DryRun) { $arguments += '--dry-run' }
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw "Checkpoint, package/range verification or flash failed (exit $LASTEXITCODE)" }
