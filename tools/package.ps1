param([string]$Python = 'C:\Users\isaia\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $projectRoot 'firmware\build'
$packageDir = Join-Path $projectRoot 'dist\v1'
New-Item -ItemType Directory -Force $packageDir | Out-Null
$segments = @('bootloader\bootloader.bin','partition_table\partition-table.bin','watchfw.bin')
foreach ($segment in $segments) {
    Copy-Item -LiteralPath (Join-Path $buildDir $segment) -Destination $packageDir
}
& $Python -m esptool --chip esp32s3 merge_bin --output "$packageDir\watch-v1.bin" `
    --flash_mode dio --flash_size 16MB --flash_freq 80m `
    0x0 "$packageDir\bootloader.bin" 0x8000 "$packageDir\partition-table.bin" 0x10000 "$packageDir\watchfw.bin"
if ($LASTEXITCODE -ne 0) { throw 'Image merge failed' }
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\flash-package.ps1') -Destination "$packageDir\flash.ps1"
@'
# Watch OS v1 — Waveshare ESP32-S3-Touch-AMOLED-1.43-B

Built with ESP-IDF 5.5.4 for 16 MB flash and 8 MB octal PSRAM.
Hardware boot/USB test flags are in manifest.json, matched to the firmware
hashes in hardware-validation.json. Full physical validation remains incomplete.
This is for the plain 1.43 / metal-case 1.43-B, not the 1.43C.

Install the flashing utility, then replace COM7 with your board's port:

```powershell
python -m pip install 'esptool>=4.9,<5'
.\flash.ps1 -Port COM7
```

The script writes the three firmware segments, preserving NVS settings.
If automatic download fails, hold BOOT, press/release RESET, release BOOT,
and retry. Use a USB data cable and close other serial monitors first.

SHA-256 hashes and file sizes are in manifest.json. The three separate
segments are bootloader.bin at 0x0, partition-table.bin at 0x8000, and
watchfw.bin at 0x10000. The merged image is not an OTA image; writing it at 0x0
also overwrites the intervening NVS region, so use flash.ps1 for normal updates.

The source project's README.md documents setup, SD card use, USB commands,
the official hardware references, and known v1 limitations.
'@ | Set-Content -LiteralPath "$packageDir\README.md" -Encoding utf8
$records = foreach ($name in @('bootloader.bin','partition-table.bin','watchfw.bin','watch-v1.bin')) {
    $file = Get-Item -LiteralPath (Join-Path $packageDir $name)
    @{ file=$name; bytes=$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLower() }
}
$evidencePath = Join-Path $projectRoot 'docs\device-validation.json'
$evidence = $null
$hardwareMatches = Test-Path -LiteralPath $evidencePath
if ($hardwareMatches) {
    $evidence = Get-Content -LiteralPath $evidencePath -Raw | ConvertFrom-Json
    foreach ($record in $records | Where-Object { $_.file -ne 'watch-v1.bin' }) {
        $tested = @($evidence.images | Where-Object { $_.file -eq $record.file })
        if ($tested.Count -ne 1 -or $tested[0].sha256 -ne $record.sha256) { $hardwareMatches = $false }
    }
}
@{ applies_to_packaged_images=$hardwareMatches; evidence=$evidence } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$packageDir\hardware-validation.json" -Encoding utf8
@{
    board='ESP32-S3-Touch-AMOLED-1.43-B'; idf='5.5.4'; flash_mb=16
    built_utc=[DateTime]::UtcNow.ToString('o'); hardware_validated=$false
    hardware_boot_tested=($hardwareMatches -and $evidence.boot_tested)
    hardware_usb_tested=($hardwareMatches -and $evidence.usb_smoke_tested)
    hardware_evidence='hardware-validation.json'
    merged_flash_offset='0x0'; files=@($records)
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$packageDir\manifest.json" -Encoding utf8
Write-Host "Packaged $packageDir"
