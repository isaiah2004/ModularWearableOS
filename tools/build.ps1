param(
    [string]$IdfPath = 'A:\makerspace\esp-idf',
    [string]$Port,
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Python
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if ($Flash -and -not $Port) { throw '-Port is required for a checked preference-preserving flash' }
$temporaryRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot '.worktrees/_audit_tmp'))
if (-not $temporaryRoot.StartsWith(([IO.Path]::GetFullPath($projectRoot) + [IO.Path]::DirectorySeparatorChar), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Temporary output escaped the workspace'
}
New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
foreach ($candidate in @((Join-Path $projectRoot '.worktrees'), $temporaryRoot)) {
    if ((Get-Item -LiteralPath $candidate).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Temporary directories must stay on the workspace volume; external junctions/symlinks are not supported'
    }
}
$env:TEMP = $temporaryRoot
$env:TMP = $temporaryRoot
$env:TMPDIR = $temporaryRoot
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    # Tool launchers can be the nearest non-Python parent (for example mise).
    # The executing shell is known here; avoid SDK process-tree guessing.
    $previousIdfShell = $env:ESP_IDF_SHELL
    $env:ESP_IDF_SHELL = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh' } else { 'powershell' }
    try { . (Join-Path $IdfPath 'export.ps1') }
    finally { $env:ESP_IDF_SHELL = $previousIdfShell }
}
if (-not $Python) {
    if ($env:IDF_PYTHON_ENV_PATH) {
        $idfPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
        if (Test-Path -LiteralPath $idfPython) { $Python = $idfPython }
    }
    if (-not $Python) { $Python = (Get-Command python -ErrorAction Stop).Source }
}
$idfScript = Join-Path $IdfPath 'tools/idf.py'
$identityScript = Join-Path $PSScriptRoot 'build_identity.py'
Push-Location (Join-Path $projectRoot 'firmware')
try {
    # Resolve generated config before taking the immutable current-source snapshot.
    & $Python $idfScript reconfigure
    if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF configure failed' }
    & $Python $identityScript prepare
    if ($LASTEXITCODE -ne 0) { throw 'Source/config/asset identity snapshot failed' }
    & $Python $idfScript build
    if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF build failed' }
    & $Python $identityScript finalize
    if ($LASTEXITCODE -ne 0) { throw 'Build/source/feature/partition verification failed; do not flash this output' }
    if ($Flash) {
        & $Python (Join-Path $PSScriptRoot 'flash_watch.py') --port $Port
        if ($LASTEXITCODE -ne 0) { throw 'Preference checkpoint or flash failed' }
    }
    if ($Monitor) {
        if (-not $Port) { throw '-Port is required for monitor' }
        & $Python $idfScript -p $Port monitor
        if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF monitor failed' }
    }
} finally { Pop-Location }
