# Current NOS simulator. Every profile is strict and has a content manifest.
param([string]$Out = 'nos_sim.exe', [switch]$Run, [switch]$Verify, [switch]$Debug,
      [switch]$Clean, [switch]$NoCostModel, [switch]$Strict, [string]$Python = 'python',
      [string]$Compiler)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$arguments = @((Join-Path $root 'tools/build_sim.py'), '--out', $Out)
if ($Verify) { $arguments += '--verify' }
if ($Debug) { $arguments += '--debug' }
if ($Clean) { $arguments += '--clean' }
if ($NoCostModel) { $arguments += '--no-cost' }
if ($Compiler) { $arguments += @('--compiler', $Compiler) }
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw "Strict NOS simulator build failed (exit $LASTEXITCODE)" }
if ($Run) { Start-Process -FilePath (Join-Path $root "build/$Out") -WorkingDirectory $root -WindowStyle Normal }
