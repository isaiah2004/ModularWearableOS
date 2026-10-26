# Simulator-only Sports validation. No device connection or flash operation.
param(
    [switch]$SkipBuild,
    [switch]$NoMedia,
    [string]$Simulator = 'build/nos_sports.exe'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compiler = (Get-Command gcc -ErrorAction Stop).Source
$testOutput = Join-Path $projectRoot ('build/workout-checks/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
Push-Location $projectRoot
try {
    $simulatorPath = if ([IO.Path]::IsPathRooted($Simulator)) { [IO.Path]::GetFullPath($Simulator) }
                     else { [IO.Path]::GetFullPath((Join-Path $projectRoot $Simulator)) }
    if (-not $SkipBuild) {
        $simulatorName = [IO.Path]::GetFileName($Simulator)
        if ($simulatorPath -ne
            [IO.Path]::GetFullPath((Join-Path $projectRoot ('build/' + $simulatorName)))) {
            throw 'Build output must be a file directly under build/. Use -SkipBuild for an existing external simulator.'
        }
        & (Join-Path $projectRoot 'sim/nos_build.ps1') -Strict -Out $simulatorName
        if ($LASTEXITCODE -ne 0) { throw 'Strict simulator build failed' }
    }
    if (-not (Test-Path -LiteralPath $simulatorPath -PathType Leaf)) { throw "Simulator missing: $simulatorPath" }
    $engineTest = Join-Path $testOutput 'workout_test.exe'
    & $compiler -std=c99 -O2 -Wall -Wextra -Werror "-I$projectRoot/core" "-I$projectRoot/nos" "$projectRoot/tests/workout_test.c" -lm -o $engineTest
    if ($LASTEXITCODE -ne 0) { throw 'Workout engine test compilation failed' }
    Push-Location $testOutput
    try {
        & $engineTest
        if ($LASTEXITCODE -ne 0) { throw 'Workout engine tests failed' }
        $recoveryStore = Join-Path $testOutput 'process-recovery'
        & $engineTest --write-recovery $recoveryStore
        if ($LASTEXITCODE -ne 0) { throw 'Recovery checkpoint creation failed' }
        & $engineTest --read-recovery $recoveryStore
        if ($LASTEXITCODE -ne 0) { throw 'Separate-process recovery test failed' }
    } finally { Pop-Location }
    python -m unittest discover -s tests -p test_workouts_export.py -v
    if ($LASTEXITCODE -ne 0) { throw 'Workout export tests failed' }
    $suiteArgs = @('tools/test_workout_sim.py', '--sim', $simulatorPath, '--out', (Join-Path $testOutput 'sim'))
    if ($NoMedia) { $suiteArgs += '--no-media' }
    python @suiteArgs
    if ($LASTEXITCODE -ne 0) { throw "Simulator acceptance failed. Evidence: $testOutput" }
    Write-Host "PASS Sports engine, export, simulator, motion and damage checks. Evidence: $testOutput"
} finally { Pop-Location }
