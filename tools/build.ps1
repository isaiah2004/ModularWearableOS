param(
    [string]$IdfPath = 'A:\makerspace\esp-idf',
    [string]$Port,
    [switch]$Flash,
    [switch]$Monitor
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    . (Join-Path $IdfPath 'export.ps1')
}
Push-Location (Join-Path $projectRoot 'firmware')
try {
    $idfArgs = @()
    if ($Port) { $idfArgs += @('-p', $Port) }
    $idfArgs += 'build'
    if ($Flash) { $idfArgs += 'flash' }
    if ($Monitor) { $idfArgs += 'monitor' }
    idf.py @idfArgs
    if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF build/flash failed' }
} finally { Pop-Location }
