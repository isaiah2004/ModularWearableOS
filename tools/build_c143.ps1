# Build (and optionally flash) the ESP32-S3-Touch-AMOLED-1.43C variant into
# firmware/build_c143 with its own sdkconfig.c143. The default 1.43-B build
# (firmware/build, firmware/sdkconfig) is not touched.
#   tools\build_c143.ps1                 # build only
#   tools\build_c143.ps1 -Port COM4      # build + flash
param([string]$Port = '', [switch]$Monitor)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$py = 'C:\Users\isaia\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe'
$env:IDF_PATH = 'A:\makerspace\esp-idf'
$kv = & $py "$env:IDF_PATH\tools\idf_tools.py" export --format key-value 2>$null
foreach ($l in $kv) {
    $k, $v = $l -split '=', 2
    if ($k) { Set-Item "env:$k" ($v -replace '%PATH%', $env:PATH -replace '\$PATH', $env:PATH) }
}
$env:PATH = "C:\Users\isaia\.espressif\python_env\idf5.5_py3.12_env\Scripts;$env:PATH"
$idfArgs = @('-B', 'build_c143', '-D', 'SDKCONFIG=sdkconfig.c143',
    '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.c143')
if ($Port) { $idfArgs += @('-p', $Port, 'build', 'flash') } else { $idfArgs += 'build' }
if ($Monitor) { $idfArgs += 'monitor' }
Push-Location "$projectRoot\firmware"
try {
    & $py "$env:IDF_PATH\tools\idf.py" @idfArgs
    if ($LASTEXITCODE -ne 0) { throw "idf.py failed ($LASTEXITCODE)" }
} finally { Pop-Location }
