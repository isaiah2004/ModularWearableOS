$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
Push-Location $root
try {
    python tools/orrery/ringread/review.py
    if ($LASTEXITCODE) { throw 'Ring-final review failed' }
} finally { Pop-Location }
