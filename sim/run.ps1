# Build and open current NOS; existing simulators are never killed by name.
param([string]$Out = 'nos_sim.exe', [string]$Python = 'python')
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'nos_build.ps1') -Strict -Out $Out -Python $Python -Run
if (-not $?) { throw 'NOS simulator did not build or start' }
