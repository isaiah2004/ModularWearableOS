$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compiler = (Get-Command gcc -ErrorAction Stop).Source
$outDir = Join-Path $projectRoot 'build\optimization-tests'
New-Item -ItemType Directory -Force $outDir | Out-Null
$cases = @(
    @{ Name='blur_cache'; Sources=@('tests/blur_cache_test.c','nos/blur_cache.c') },
    @{ Name='ui_blur_cache'; Sources=@('tests/ui_blur_cache_test.c','nos/ui_blur.c','nos/blur_cache.c','core/gfx.c','core/sprite.c') },
    @{ Name='ui_blur_raster'; Sources=@('tests/ui_blur_raster_test.c','nos/blur_cache.c','core/gfx.c','core/sprite.c','core/font.c','core/fonts/font_small.c') },
    @{ Name='dot_mask_race'; Sources=@('tests/dot_mask_race_test.c','nos/dot_glyphs.c','core/gfx.c') },
    @{ Name='nos_frame_perf'; Sources=@('tests/nos_frame_perf_test.c') },
    @{ Name='nos_frame_pacer'; Sources=@('tests/nos_frame_pacer_test.c') },
    @{ Name='nos_button_pulse'; Sources=@('tests/nos_button_pulse_test.c') },
    @{ Name='nos_prefs'; Sources=@('tests/nos_prefs_test.c','nos/prefs.c') },
    @{ Name='prefs_disk'; Sources=@('tests/prefs_disk_test.c','nos/prefs_disk.c'); Directory=$true },
    @{ Name='nos_render_bounds'; Sources=@('tests/nos_render_bounds_test.c','core/gfx.c') },
    @{ Name='sleep_grid'; Sources=@('tests/sleep_grid_test.c') }
)
foreach ($case in $cases) {
    $sources = @($case.Sources | ForEach-Object { Join-Path $projectRoot $_ })
    $exe = Join-Path $outDir ($case.Name + '.exe')
    & $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" "-I$projectRoot\nos" "-I$projectRoot\firmware\main" @sources -lm -o $exe
    if ($LASTEXITCODE -ne 0) { throw "$($case.Name) compilation failed" }
    if ($case.Directory) {
        $testDir = Join-Path $outDir ('prefs-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $testDir | Out-Null
        & $exe $testDir
    } else { & $exe }
    if ($LASTEXITCODE -ne 0) { throw "$($case.Name) test failed" }
}
& "$outDir\ui_blur_cache.exe" --allocation-failure
if ($LASTEXITCODE -ne 0) { throw 'UI blur allocation failure test failed' }
$powerExe = Join-Path $outDir 'nos_power.exe'
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\tests\stubs\nos_power" "-I$projectRoot\firmware\main" "$projectRoot\tests\nos_power_test.c" "$projectRoot\firmware\main\nos_power.c" -o $powerExe
if ($LASTEXITCODE -ne 0) { throw 'Power lock test compilation failed' }
foreach ($mode in @('normal','config-fail','dfs-fail','create-fail','second-create-fail','third-create-fail','fourth-create-fail','acquire-fail','display-acquire-fail','sleep-fail','runtime-acquire-fail','runtime-release-fail')) {
    & $powerExe $mode
    if ($LASTEXITCODE -ne 0) { throw "Power lock test failed: $mode" }
}
Push-Location $projectRoot
try {
    python tools/generate_native_menu_assets.py --verify
    if ($LASTEXITCODE -ne 0) { throw 'Baked native menu artwork verification failed' }
    python tools/test_orrery_sd.py
    if ($LASTEXITCODE -ne 0) { throw 'Orrery sky pack verification failed' }
    python -m unittest discover -s tests -p 'test_nos_*.py' -v
    if ($LASTEXITCODE -ne 0) { throw 'Optimization Python tests failed' }
    python -m unittest discover -s tests -p 'test_backup_flash.py' -v
    if ($LASTEXITCODE -ne 0) { throw 'Flash backup protocol tests failed' }
    foreach ($pattern in @('test_async_assets.py','test_sd_asset_leases.py','test_orrery_shade_q16.py','test_card_prepare_incremental.py','test_launcher_continuity.py','test_launcher_rom.py','test_native_menu_motion.py','test_native_asset_packing_budget.py','test_settings_sharp_pack.py')) {
        python -m unittest discover -s tests -p $pattern -v
        if ($LASTEXITCODE -ne 0) { throw "Standalone optimization tests failed: $pattern" }
    }
} finally { Pop-Location }
