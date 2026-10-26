# Current validation entry point

Use [HOST_VALIDATION.md](HOST_VALIDATION.md) and `tools/validation_matrix.json` for
current NOS v0.5 Alpha. Run `python tools/validate.py --profile host`, then the
full offline profile with an explicitly built strict NOS simulator. Each run
retains machine-readable results, exact test inventory, failures and skips.
Hardware runs require an explicit port and expected ELF identity and are never
part of offline test import/discovery. Image sizes/hashes below are historical.

# Historical validation record — 2026-09-12

## Touch-test follow-up

The current build adds `diag touch`: five targets, live crosshair, transformed
coordinates, down/up state and tap count. It also preserves the last valid UI
coordinate when FT3168 reports release or a read fails. Previously release
coordinates bypassed the rotation and could make a tap look like a swipe.
The press/release regression test and firmware build pass. This build was
flashed, booted and commanded to open the touch page; it remains open with
`awake 1` for physical testing. The simulator touch-page layout was inspected.

Current image size is 6,341,408 bytes. Current hashes and precise test scope
are in `device-validation.json` and the package manifest. The full 25-app/
13-face tour recorded below belongs to the preceding rotation build; this
follow-up does not repeat that tour while the user is testing physical touch.
Evidence: `build/touch-test-boot.log`, `build/usb-touch-test.jsonl`,
`build/flash-touch-test.log`, and `build/diag_touch.bmp`.

## Completed locally

| Check | Result |
|---|---|
| Full ESP-IDF build | PASS, ESP-IDF 5.5.4, Xtensa GCC 14.2.0, target esp32s3 |
| Final incremental firmware build | PASS; no warnings from changed firmware modules |
| Application image | 6,340,448 bytes, 6.047 MiB; 60% free in 0xEF0000 app partition |
| Flash settings | 16 MB, DIO, 80 MHz; 8 MB octal PSRAM configured |
| Native USB console | USB Serial/JTAG selected in generated sdkconfig |
| FATFS settings | Long filenames in heap; no auto-format; SDMMC 1-bit/20 MHz |
| Windows simulator build | PASS; inherited unused/missing-initializer warnings remain |
| Board protocol tests | PASS, C99 with `-Wall -Wextra -Werror` |
| RTC tests | Round trips for every supported day 2000..2099; leap day, 2038, range and malformed BCD/oscillator-stop cases |
| Touch/rotation tests | Native edges/releases, every screen pixel through output/touch inverse, non-square pixel rotation with canaries, one-row and one-column cases |
| IMU tests | Signed little-endian values and g/degree-per-second scaling |
| USB host utility tests | 3 PASS: response/log interleaving, timeout, oversized/multiline rejection |
| Simulator screenshot run | PASS, 42 BMPs: apps, four overlays, thirteen faces |
| Visual inspection | Storage, Recorder unavailable state, watchface, diagnostics statuses checked |
| Extra diagnostic screenshots | Four pages generated with `sim.exe --diag` |
| Flash package | SHA-256/size checks PASS; app at 0x10000 and partition table at 0x8000 match source segments |
| esptool application inspection | Valid image checksum and validation hash |

Evidence files: root `build-firmware.log`, `build-firmware-final.log`,
`build-simulator-final.log`; `build/screenshots/`; `dist/v1/manifest.json`.
Generated build artifacts and screenshots are intentionally ignored by Git.

Reproduce the protocol/USB tests with:

```powershell
.\tools\test.ps1
python -m unittest discover -s tests -p 'test_*.py'
```

The USB utility uses pyserial 3.5. Boot capture includes esptool's Windows
usbser.sys workaround: repeat DTR after changing RTS so the USB control request
actually resets the chip. No user settings are erased by the capture tool.

## Completed on the connected hardware

| Check | Result |
|---|---|
| Identity | COM3, native USB 303A:1001; ESP32-S3 rev 0.2, 16 MB flash, 8 MB embedded PSRAM. Backed-up factory app is `example_qspi_with_ram` with the plain/B board drivers |
| Recovery backup | Complete 16 MB read and verified before any flash; see [recovery](RECOVERY.md) |
| Flash | Final bootloader, partition table and application written separately; all three device hashes verified; NVS region preserved |
| Boot | PASS; `watch os running (466x466)` at about 2.4 seconds; PSRAM memory test OK; no panic in the captured runs |
| Panel selection | Read ID 0xff; selected CO5300 with x gap 6 using the official fallback. This does not independently identify the controller or prove the image looks correct |
| I2C | Touch 0x38, RTC 0x51, and IMU 0x6b all respond; IMU WHO_AM_I check passes |
| Motion data | Valid live samples, approximately `(0.08, 0.05, -1.00)` g while stationary. Gyro bias and screen-axis alignment remain to be calibrated/checked |
| Memory adjustment | Rotation uses two 16-row input stripes plus one DMA output stripe, about 44 KiB total. Free internal memory at startup is 80 KiB; runtime heap metrics included in `board` |
| Clock | USB set local time/IST +330; RTC restored after firmware update/reset, within one second of the PC. No battery-disconnected retention test |
| USB/UI exercise | All 25 app IDs and 13 face selections, four edge overlays, AOD toggle, pause/resume, Diagnostics, Level and Storage complete without a command timeout or panic |
| Input rejection | Invalid calendar, negative touch coordinate, and out-of-range app ID return errors and keep the command interface usable |
| Radio startup | Wi-Fi setup AP and NimBLE scan/advertising start; pairing, network join, and media transfer not tested |
| No mounted SD | Boot continues after SD timeout; status shows unmounted; `sdtest` correctly returns ESP_ERR_INVALID_STATE. No card contents were formatted or written |

Final firmware SHA-256:
`08166f46ab5383f2c1a7f94f4c2063a2d3a37b9d2ada53efb2046a8205c2fdd2`.
Rotation-build evidence: `build/rotation-boot.log`, `build/flash-rotation.log`,
`build/usb-smoke-rotation.jsonl`, `build/usb-tour-rotation.jsonl`,
and `build/build-rotation.log`. The original RTC retention measurement remains
in `build/rtc-retention.json` from the previous build.
The source test runner is retained locally as `build/device-tour.py`;
the reusable public USB smoke command is documented in [USB control](USB_CONTROL.md).

### Orientation correction

The user reported that the first display was rotated 90 degrees clockwise.
The corrected build rotates pixel output counterclockwise and applies the
inverse transform to physical touch. USB gesture coordinates remain in the
upright UI coordinate system. The official driver rejects `swap_xy`, so pixel
rotation is performed in a separate buffer whose reuse waits for DMA completion.
The correction requires visual confirmation on the watch, including touch
alignment; IMU axis alignment is still a separate physical check.

## Physical checks still required

1. Inspect RGB colours, round bounds and touch position in all four quadrants;
   confirm the selected panel configuration renders correctly.
2. Run `board` and `imu`; tilt the board and verify live signed motion values.
   Check Diagnostics/Level visually. IMU-to-screen axis orientation may need tuning.
3. Repeat time synchronization with Wi-Fi/BLE and test retention using the RTC
   backup supply, if fitted.
4. Boot with a FAT32 card. Confirm it reaches the UI.
   Require `SDTEST ESP_OK`; inspect `/watch/logs` after at least 60 seconds.
   Check `sdeject`, removal, reinsertion and `sdmount`.
5. Use `--smoke --log build/usb-smoke.jsonl` with the camera recording to inspect
   navigation, frame pacing, Storage, Diagnostics and Level. This command alone
   does not prove the physical UI is correct.
6. Check 30-second dim, 120-second display-off, touch/BOOT recovery, AOD,
   explicit deep sleep and BOOT wake. Measure current and system voltage with
   USB connected and on battery. No battery-life or tear-free claim is made.

Device boot and USB-controlled app state changes are tested. Physical touch,
display appearance, SD read/write, radio pairing, and battery/power measurements
remain pending; command acknowledgements do not verify these physical effects.
