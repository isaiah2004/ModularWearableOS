# Factory firmware backup

Before the first v1 flash on 2026-09-12, the complete 16 MB flash from the
connected ESP32-S3 was saved locally:

- Image: `build/backups/before-v1-verified.bin` (16,777,216 bytes)
- SHA-256: `cd2c926717b800eb635712b35ac8cc91ffd6b1912f33fcf0d1e29012105f81fc`
- Device MAC: `28:84:85:b0:8b:30`
- Verification: sixteen 1 MiB esptool reads, each checked against the
  device-supplied MD5, then SHA-256 checks while assembling the full image.
- Per-chunk checksums: `build/backups/before-v1-verified.json`

The first monolithic read failed near its end with a USB transfer error.
No firmware was written until the complete chunked backup above succeeded.
The original application identifies itself as `example_qspi_with_ram` and
contains the QMI8658, PCF85063, and LCD-probe factory driver identifiers.

Keep the backup outside Git: it includes the original NVS contents. The old
`A:\makerspace\smartwatch` source directory was left intact.

## Restore if needed

Close other serial tools, verify the backup hash, and use the same board:

```powershell
Get-FileHash build/backups/before-v1-verified.bin -Algorithm SHA256
& 'C:\Users\isaia\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe' -m esptool --chip esp32s3 --port COM3 --baud 460800 write_flash --flash_mode keep --flash_size keep --flash_freq keep 0x0 build/backups/before-v1-verified.bin
```

Restoring the full backup replaces the current firmware and NVS settings with
their pre-port contents. It does not change a fitted SD card or the external
RTC. Normal v1 updates use the three separate firmware segments instead.

To make another backup, use a new filename with `tools/backup_flash.py`
under the ESP-IDF Python environment. It resets the device but does not write
to flash. Keep the `.parts` directory and JSON beside the assembled image.

## Before the persistent Pip update

The previously flashed touch-test OS segments and manifest were preserved in
`build/backups/pre-pip-com3/`. Application SHA-256:
`abfdc93fd21a27caf4e5d0f5e2a6ddf852d05c5c11f739ae7f8ba94c9c56123b`.
The same folder contains a 24 KiB NVS read from the confirmed watch on COM3,
MAC `28:84:85:b0:8b:30`, before the Pip update. NVS SHA-256:
`bea0f92953500654ef10d2cb62e19ac7bbedccaf2c477f5e3f79be48199c41c7`.
Keep NVS private; it may contain connection settings. Routine rollback uses
the three old firmware segments and preserves NVS unless restoring old
settings is explicitly intended.

During identification, a different 8 MiB ESP32 on COM8 was briefly attached.
It received an identification command and an unsuccessful flash read only.
No firmware was written to it. It is not this watch or a recovery target.

## Before the animation optimization

`build/backups/pre-smooth/` contains the three previously tested firmware
segments, their manifest and hardware evidence, plus the source files changed
by the optimization. Previous application SHA-256:
`eb616ba33d5d937969861dc209bf6f30eabc0e6b8d556067348a6af2604c645e`.
The optimization preserves the NVS layout and care records. Rolling back with
the three separate segments preserves the watch's current settings and Pip
memory; this folder does not contain an additional NVS dump.
