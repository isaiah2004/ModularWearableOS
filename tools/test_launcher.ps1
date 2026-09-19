$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compiler = (Get-Command gcc -ErrorAction SilentlyContinue).Source
if (-not $compiler) {
    $compiler = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $compiler) { throw 'Install GCC or WinLibs first' }

$outDir = Join-Path $projectRoot 'build\tests\launcher-render'
New-Item -ItemType Directory -Force $outDir | Out-Null
$referenceCommit = '7b5c1b3'
$objects = @()
foreach ($name in @('launcher','applist')) {
    $reference = Join-Path $outDir "app_${name}_reference.c"
    git -C $projectRoot show "${referenceCommit}:core/apps/app_${name}.c" | Set-Content -Encoding utf8 $reference
    if ($LASTEXITCODE -ne 0) { throw "Could not extract app_${name}.c from $referenceCommit" }
    $refObj = Join-Path $outDir "${name}_reference.o"
    & $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core\apps" "-I$projectRoot\core" `
        "-Dapp_${name}=app_${name}_ref" -c $reference -o $refObj
    if ($LASTEXITCODE -ne 0) { throw "$name reference compilation failed" }
    $objects += $refObj
    $curObj = Join-Path $outDir "${name}_current.o"
    & $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
        -c (Join-Path $projectRoot "core\apps\app_${name}.c") -o $curObj
    if ($LASTEXITCODE -ne 0) { throw "$name current compilation failed" }
    $objects += $curObj
}
$fontSources = Get-ChildItem "$projectRoot\core\fonts\*.c" | ForEach-Object FullName
$exe = Join-Path $outDir 'launcher_render_test.exe'
& $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot\core" `
    (Join-Path $projectRoot 'tests\launcher_render_test.c') @objects `
    (Join-Path $projectRoot 'core\gfx.c') (Join-Path $projectRoot 'core\font.c') `
    (Join-Path $projectRoot 'core\icons.c') (Join-Path $projectRoot 'core\pathtext.c') `
    @fontSources -lm -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Launcher render test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Launcher render regression failed' }
