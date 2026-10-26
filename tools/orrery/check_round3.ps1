# Build and exercise the renderer without firmware, serial, or flashing.
# Requires MinGW gcc/objcopy on PATH. -SkipBuild reuses current simulator objects.
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if (-not $SkipBuild) {
        & ./sim/nos_build.ps1 -Strict -Out nos_sim_round3.exe
        & ./sim/nos_build.ps1 -Strict -NoCostModel -Out nos_sim_round3_nocost.exe
    }
    New-Item -ItemType Directory -Force build/orrery/round3_test | Out-Null
    $out = 'build/orrery/round3_test'
    & objcopy --redefine-sym main=sim_program_main build/nos_obj/sim_nos_sim.o "$out/sim.o"
    if ($LASTEXITCODE) { throw 'objcopy failed' }
    & gcc -O2 -std=c99 -DGFX_COST_MODEL -Icore -Inos -Inos/fonts -Isim -c tests/orrery_raster_test.c -o "$out/test.o"
    if ($LASTEXITCODE) { throw 'test compile failed' }
    $objects = Get-ChildItem build/nos_obj/*.o | Where-Object {
        $_.Name -notin @('sim_nos_sim.o','nos_faces_face_orrery.o')
    } | ForEach-Object FullName
    & gcc @objects "$out/sim.o" "$out/test.o" -o "$out/test.exe" -lgdi32 -luser32 -lwinmm -lm
    if ($LASTEXITCODE) { throw 'test link failed' }
    & "$out/test.exe" --script tools/orrery/perf_round3.txt --out "$out/unit" --no-media --quiet
    if ($LASTEXITCODE) { throw 'raster regression failed' }
    foreach ($script in Get-ChildItem tools/orrery/*.txt) {
        & build/nos_sim_round3.exe --script $script.FullName --out "$out/$($script.BaseName)" --no-media --quiet
        if ($LASTEXITCODE) { throw "script failed: $($script.Name)" }
    }
    foreach ($mode in @('cost','nocost')) {
        $exe = if($mode -eq 'cost') { 'build/nos_sim_round3.exe' } else { 'build/nos_sim_round3_nocost.exe' }
        & $exe --script tools/orrery/perf_round3.txt --out "$out/$mode" --hashes "$out/$mode.hash" --no-media --quiet
        if ($LASTEXITCODE) { throw "$mode run failed" }
    }
    if ((Get-FileHash "$out/cost.hash").Hash -ne (Get-FileHash "$out/nocost.hash").Hash) {
        throw 'cost hooks changed pixels'
    }
    Write-Host 'PASS ORRERY scripts, raster regressions, and cost/no-cost pixel hashes'
} finally { Pop-Location }
