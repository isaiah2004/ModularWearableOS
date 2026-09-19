param(
    [string]$OutputDirectory = 'build/ultra-preview',
    [ValidateRange(0,23)][int]$Hour = 10,
    [ValidateRange(0,59)][int]$Minute = 9,
    [ValidateRange(0,100)][int]$Battery = 72,
    [ValidateRange(0,2147483647)][int]$Steps = 6234,
    [ValidateRange(1,31)][int]$Day = 12
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compiler = (Get-Command gcc -ErrorAction SilentlyContinue).Source
if (-not $compiler) {
    $compiler = Get-ChildItem "$env:LOCALAPPDATA/Microsoft/WinGet/Packages" -Recurse -Filter gcc.exe |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $compiler) { throw 'Install GCC or WinLibs first' }
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot $OutputDirectory
}
$out = New-Item -ItemType Directory -Force $OutputDirectory
$sources = @('tools/face_preview.c','core/gfx.c','core/font.c','core/icons.c',
             'core/layer.c','core/pathtext.c','core/apps/face_ultra.c') |
    ForEach-Object { Join-Path $projectRoot $_ }
$sources += @(Get-ChildItem "$projectRoot/core/fonts/*.c" | ForEach-Object FullName)
& $compiler -O2 -std=c99 @sources -DFACEFN=face_ultra_draw -o "$out/ultra_preview.exe" -lm
if ($LASTEXITCODE -ne 0) { throw 'Ultra preview compilation failed' }
Push-Location $out.FullName
try {
    & ./ultra_preview.exe $Hour $Minute $Battery $Steps $Day
    if ($LASTEXITCODE -ne 0) { throw 'Ultra preview failed' }
    Add-Type -AssemblyName System.Drawing
    foreach ($n in 0..3) {
        $bitmap = [Drawing.Bitmap]::FromFile((Join-Path $out.FullName "preview$n.bmp"))
        try { $bitmap.Save((Join-Path $out.FullName "preview$n.png"), [Drawing.Imaging.ImageFormat]::Png) }
        finally { $bitmap.Dispose() }
    }
} finally { Pop-Location }
Write-Host "Fresh Ultra previews: $($out.FullName)"
