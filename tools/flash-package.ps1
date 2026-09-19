param([Parameter(Mandatory=$true)][string]$Port, [string]$Python='python')
$ErrorActionPreference = 'Stop'
# Run from the dist/v1 package, where this script is named flash.ps1.
# Install the flashing utility once: python -m pip install 'esptool>=4.9,<5'
& $Python -m esptool --chip esp32s3 --port $Port --baud 460800 `
    --before default_reset --after hard_reset write_flash `
    --flash_mode dio --flash_size 16MB --flash_freq 80m `
    0x0 "$PSScriptRoot\bootloader.bin" 0x8000 "$PSScriptRoot\partition-table.bin" `
    0x10000 "$PSScriptRoot\watchfw.bin"
if ($LASTEXITCODE -ne 0) { throw 'Flash failed; check COM port and BOOT/RESET procedure' }
