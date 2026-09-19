$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compiler = (Get-Command gcc -ErrorAction SilentlyContinue).Source
if (-not $compiler) {
    $compiler = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $compiler) { throw 'Install GCC or WinLibs first' }
$outDir = Join-Path $projectRoot 'build\tests'
New-Item -ItemType Directory -Force $outDir | Out-Null
& $compiler -std=c99 -O2 -Wall -Wextra -Werror `
    "-I$projectRoot\core" "-I$projectRoot\firmware\main" `
    "$projectRoot\tests\board_protocol_test.c" "$projectRoot\firmware\main\board_protocol.c" `
    -o "$outDir\board_protocol_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed' }
& "$outDir\board_protocol_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Board protocol test failed' }
& $compiler -std=c99 -O2 -Wall -Wextra -Werror `
    "-I$projectRoot\tests\stubs" "-I$projectRoot\firmware\main" `
    "$projectRoot\tests\display_perf_test.c" "$projectRoot\firmware\main\display_perf.c" `
    -o "$outDir\display_perf_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Display performance test compilation failed' }
& "$outDir\display_perf_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Display performance test failed' }
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
    "$projectRoot\tests\pip_test.c" "$projectRoot\core\pip.c" -lm -o "$outDir\pip_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Pip test compilation failed' }
& "$outDir\pip_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Pip test failed' }
foreach ($module in @('pip_care', 'walk', 'notif', 'battery', 'motion_wake')) {
    & $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
        "$projectRoot\tests\${module}_test.c" "$projectRoot\core\${module}.c" -lm -o "$outDir\${module}_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "$module test compilation failed" }
    & "$outDir\${module}_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "$module test failed" }
}
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
    "$projectRoot\tests\pedometer_test.c" "$projectRoot\core\pedometer.c" "$projectRoot\core\walk.c" `
    -lm -o "$outDir\pedometer_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Pedometer test compilation failed' }
& "$outDir\pedometer_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Pedometer test failed' }
$cjson = 'A:\makerspace\esp-idf\components\json\cJSON'
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" "-I$projectRoot\firmware\main" "-I$cjson" `
    "$projectRoot\tests\weather_parse_test.c" "$projectRoot\firmware\main\weather_parse.c" "$cjson\cJSON.c" `
    -lm -o "$outDir\weather_parse_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Weather parser test compilation failed' }
& "$outDir\weather_parse_test.exe" "$projectRoot\tests\fixtures\weather.json"
if ($LASTEXITCODE -ne 0) { throw 'Weather parser test failed' }
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
    "$projectRoot\tests\wake_service_test.c" "$projectRoot\core\wake_service.c" "$projectRoot\core\motion_wake.c" `
    -lm -o "$outDir\wake_service_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Wake service test compilation failed' }
& "$outDir\wake_service_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Wake service test failed' }
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
    "$projectRoot\tests\pip_eye_shape_test.c" "$projectRoot\core\gfx.c" -lm -o "$outDir\pip_eye_shape_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Eye raster test compilation failed' }
& "$outDir\pip_eye_shape_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Eye raster test failed' }
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
    "$projectRoot\tests\pip_eye_lid_test.c" "$projectRoot\core\gfx.c" -lm -o "$outDir\pip_eye_lid_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Lid raster test compilation failed' }
& "$outDir\pip_eye_lid_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Lid raster test failed' }
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
    "$projectRoot\tests\arc_sweep_test.c" "$projectRoot\core\gfx.c" -lm -o "$outDir\arc_sweep_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Arc sweep test compilation failed' }
& "$outDir\arc_sweep_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Arc sweep test failed' }
$ultraSources = @('tests/ultra_test.c','core/apps/face_ultra.c','core/pathtext.c',
                  'core/gfx.c','core/layer.c','core/icons.c','core/font.c') |
    ForEach-Object { Join-Path $projectRoot $_ }
$ultraSources += @(Get-ChildItem "$projectRoot/core/fonts/*.c" | ForEach-Object FullName)
& $compiler -std=c99 -O2 -Wall -Wextra -Werror @ultraSources -lm -o "$outDir/ultra_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Ultra test compilation failed' }
& "$outDir/ultra_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Ultra render test failed' }
& python -m unittest "$projectRoot\tests\test_display_perf.py" "$projectRoot\tests\test_display_perf_report.py"
if ($LASTEXITCODE -ne 0) { throw 'Display performance Python tests failed' }
