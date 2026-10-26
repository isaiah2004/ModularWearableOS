# Build and exercise the renderer without firmware, serial, or flashing.
# Requires MinGW gcc/objcopy on PATH. -SkipBuild reuses current simulator objects.
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5 turns harmless native stderr warnings into exceptions
# when this runner is redirected. Exit status is the simulator's test result.
function Invoke-OrrerySim([string]$exe, [string[]]$simArgs) {
    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $exe @simArgs
        if ($LASTEXITCODE) { throw "Simulator failed ($LASTEXITCODE): $exe $simArgs" }
    } finally { $ErrorActionPreference = $saved }
}
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if (-not $SkipBuild) {
        & ./sim/nos_build.ps1 -Strict -Out nos_sim_round6.exe
        & ./sim/nos_build.ps1 -Strict -NoCostModel -Out nos_sim_round6_nocost.exe
    }
    New-Item -ItemType Directory -Force build/orrery/round6_test | Out-Null
    $out = 'build/orrery/round6_test'
    & objcopy --redefine-sym main=sim_program_main build/nos_obj/sim_nos_sim.o "$out/sim.o"
    if ($LASTEXITCODE) { throw 'objcopy failed' }
    & gcc -O2 -std=c99 -DGFX_COST_MODEL -Icore -Inos -Inos/fonts -Isim -c tests/orrery_raster_test.c -o "$out/test.o"
    if ($LASTEXITCODE) { throw 'test compile failed' }
    $objects = Get-ChildItem build/nos_obj/*.o | Where-Object {
        $_.Name -notin @('sim_nos_sim.o','nos_faces_face_orrery.o')
    } | ForEach-Object FullName
    & gcc @objects "$out/sim.o" "$out/test.o" -o "$out/test.exe" -lgdi32 -luser32 -lwinmm -lm
    if ($LASTEXITCODE) { throw 'test link failed' }
    Invoke-OrrerySim "$out/test.exe" @("--script", "tools/orrery/perf_round6.txt", "--out", "$out/unit", "--no-media", "--quiet")
    if ($LASTEXITCODE) { throw 'raster regression failed' }
    foreach ($script in Get-ChildItem tools/orrery/*.txt) {
        Invoke-OrrerySim "build/nos_sim_round6.exe" @("--script", $script.FullName, "--out", "$out/$($script.BaseName)", "--no-media", "--quiet")
        if ($LASTEXITCODE) { throw "script failed: $($script.Name)" }
    }
    foreach ($mode in @('cost','nocost')) {
        $exe = if($mode -eq 'cost') { 'build/nos_sim_round6.exe' } else { 'build/nos_sim_round6_nocost.exe' }
        Invoke-OrrerySim $exe @("--script", "tools/orrery/perf_round6.txt", "--out", "$out/$mode", "--hashes", "$out/$mode.hash", "--no-media", "--quiet")
        if ($LASTEXITCODE) { throw "$mode run failed" }
    }
    if ((Get-FileHash "$out/cost.hash").Hash -ne (Get-FileHash "$out/nocost.hash").Hash) {
        throw 'cost hooks changed pixels'
    }
    Write-Host 'PASS ORRERY scripts, raster regressions, and cost/no-cost pixel hashes'
} finally { Pop-Location }
