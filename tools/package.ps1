param([string]$Python = 'python', [string]$Out)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$arguments = @((Join-Path $PSScriptRoot 'package_build.py'))
if ($Out) { $arguments += @('--out', $Out) }
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw "Verified build packaging failed (exit $LASTEXITCODE)" }
