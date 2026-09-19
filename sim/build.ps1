# Build the watch simulator. Usage: .\sim\build.ps1  (from repo root or sim/)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

# WinLibs GCC installed via winget; fall back to gcc on PATH
$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }

New-Item -ItemType Directory -Force "$root\build" | Out-Null

$srcs = @(
    Get-ChildItem "$root\core\*.c"
    Get-ChildItem "$root\core\apps\*.c"
    Get-ChildItem "$root\core\fonts\*.c"
    "$root\sim\main_win32.c"
) | ForEach-Object { "$_" }
# build.ps1 auto-discovers core/*.c and core/apps/*.c, so new modules
# (anim.c, notif.c, the overlay apps) are picked up with no edits here.

& $gcc -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter `
    @srcs -o "$root\build\sim.exe" -lgdi32 -luser32 -lwinmm -lm
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built $root\build\sim.exe"
