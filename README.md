# Watch OS v1 — Waveshare 1.43-B

Port of `A:\makerspace\smartwatch` into this independent workspace for the
**ESP32-S3-Touch-AMOLED-1.43-B**, the metal-case version of the plain 1.43.
The older directory is unchanged. Its actual firmware targeted the **1.43C**;
the B and C boards have different wiring and peripherals.

The existing portable renderer, 13 watch faces, navigation, apps, Wi-Fi/BLE,
and companion media protocol are retained. This port adds the B board's
SH8601/CO5300 display setup, FT3168 touch, QMI8658 readings, PCF85063 clock,
16 MB flash configuration, SD storage page/logging, and queued USB controls.

**Status:** built with ESP-IDF **5.5.4**, flashed to the connected B board on
**COM3**, and boot/USB tested on 2026-09-13. All 26 app states, 13 watch faces,
four overlays, live IMU readings, and RTC retention across a firmware update
passed the automated checks. The original 16 MB firmware is backed up.
Visual display/touch alignment, a fitted SD card, pairing, and power measurements
remain unverified. See [validation](docs/VALIDATION.md) and [recovery](docs/RECOVERY.md).

## Build and flash

From this directory in PowerShell:

```powershell
.\tools\build.ps1
.\tools\build.ps1 -Port COM7 -Flash -Monitor
```

Replace `COM7` with the device port. The script uses the existing
`A:\makerspace\esp-idf` installation; use `-IdfPath` to select another ESP-IDF
5.5.x installation. First build downloads the pinned official Waveshare panel
component. `firmware/dependencies.lock` records the resolved dependencies.

Use a USB-C **data** cable. If the port is missing or automatic download fails,
hold BOOT, press/release RESET, then release BOOT and retry. Exit the monitor
with Ctrl+] before opening another USB tool. Flashing replaces the board's
current firmware; no erase-all operation is needed.

The packaged image and segment files are under `dist/v1/`. The supplied
`flash.ps1 -Port COM7` there flashes the three firmware segments using Python
esptool, preserving NVS settings; no ESP-IDF build is needed for that route.
The tested device is currently COM3. Display output rotates 90 degrees
counterclockwise to correct the reported mounting orientation, with matching
physical touch coordinates. Two render buffers and one rotated DMA buffer use
16-row stripes (about 44 KiB total internal RAM).

## microSD card

Recommended: **32 GB SanDisk High Endurance microSDHC**, Class 10/U3/V30,
formatted **FAT32**, one partition. A genuine 16 or 32 GB Class 10 microSDHC is
also a reasonable choice. **64 GB microSDXC**, including the SanDisk Ultra GO,
should also work after reformatting to FAT32; 32 GB is a convenience recommendation,
not a firmware limit. exFAT is not enabled in this v1. No external SD module is needed: use the onboard TF
slot. The full-size SD adapter included with some cards is for a PC reader.

- Insert the card with the board powered off, then boot.
- The watch mounts it at `/sdcard` using the official example's one-bit SDMMC
  wiring, conservatively clocked at 20 MHz.
- Firmware never formats a card. Missing or unmountable cards do not stop boot.
- The **Storage** app shows capacity/free space. CSV logs go to
  `/watch/logs/NNNN.csv` every 60 seconds, with time, uptime, system voltage and
  IMU samples. Files rotate at 1 MiB; existing files are not overwritten.
- Run `sdtest` for a write/read/compare test. It creates and removes a unique
  temporary file inside `/watch`, leaving existing files alone.
- Before removing a powered card, run `sdeject` and wait for `SDEJECT ESP_OK`.
  Reinsertion requires `sdmount` or a reboot. Automatic hot-plug is not implemented.

[Card specifications](https://www.sandisk.com/products/memory-cards/microsd-cards/sandisk-high-endurance-uhs-i-microsd?capacity=32GB)
and [SDHC/FAT32 guidance](https://support-en.sandisk.com/app/answers/detailweb/a_id/9063).

## USB control and first checks

```powershell
python -m pip install pyserial
python tools/watchctl.py --list
python tools/watchctl.py --port COM7 board
python tools/watchctl.py --port COM7 --sync-time
python tools/watchctl.py --port COM7 sdtest
python tools/watchctl.py --port COM7 --smoke --log build/usb-smoke.jsonl
```

The smoke sequence checks status and exercises the UI; it does not assert that
the physical screen looks correct. Its timestamped log can be paired with a
camera recording later. See [all USB commands](docs/USB_CONTROL.md).

RTC time is stored in **UTC**, with a saved display timezone (default IST).
An unset/stopped RTC is reported as unsynchronised; use the USB command above,
Settings, BLE companion, or Wi-Fi/NTP to set it. RTC retention needs power from
the board or its RTC backup supply; it cannot retain time without either.

## Simulator and tests

```powershell
.\sim\build.ps1
.\build\sim.exe
.\tools\test.ps1
python -m unittest discover -s tests -p 'test_*.py'
```

`sim.exe --shot` saves all app, overlay and face screenshots in its working
directory. Simulator sensor values are synthetic; SD access is device-only.

## Pip's persistent personality

Pip now has 21 procedural eye performances, persistent care needs, repairable
grumpiness, qualified power reactions, estimated walking rewards, local-time
routines, and notification/alarm awareness. Select it with `face 1`; tap the
line beneath its eyes for **Pet care**. See [Living with Pip](docs/PIP_LIFE.md)
and the [case studies and revised plan](docs/research/PET_IMPLEMENTATION_PLAN.md).

## Motion wake and battery

**2026-09-15 live-data update:** daily pedometer persistence, live HTTPS weather
and feels-like temperature, NTP time sync, saved USB brightness, qualified
Ultra battery display and the square SpO2 tip are implemented. See
[Live data and IMU proposal](docs/LIVE_DATA.md) for setup and verification.

Pickup and wrist-turn wake are available under **Settings → Wake gestures**,
with left wrist selected by default and a separate learned reading pose for
each wrist. See [IMU readings and motion wake](docs/MOTION_WAKE.md). Battery
fit research, charging constraints and the day-long power budget are in
[BATTERY_FIT.md](docs/BATTERY_FIT.md).

## v1 limits

- The B board has **no onboard microphone, speaker, or vibration motor**.
  Recorder/Assistant display a microphone-needed screen. Local tones are silent;
  timer/alarm/find-watch feedback remains visual. Music control of the paired
  phone/PC remains available through the existing bridge protocol.
- No TE wire is routed from the panel. Frame pacing caps at 60 Hz, with PSRAM
  composition for large redraws; actual speed and tearing need hardware measurement.
- The voltage input measures the **system rail**, including USB power. The
  displayed battery percentage is an inherited approximation, not a fuel gauge.
- Walking-like cadence is estimated from the IMU; physical calibration is pending.
  Incoming notifications use the documented USB/HTTP/BLE companion protocol;
  automatic phone forwarding requires a sender. Deep-sleep motion wake, local audio recording,
  assistant backend, OTA, and SD media playback are not implemented. Existing
  Activity calorie/distance metrics remain estimates; active minutes use measured
  cadence duration. Weather uses Open-Meteo over Wi-Fi. Touch polling wakes a dimmed display; BOOT wakes
  explicit deep sleep. Pickup and wrist-turn wake use software polling while
  the display is dim/off or in AOD; sensitivity still needs wearing trials.
- Wi-Fi setup and the existing local media web endpoints are development features.
  The existing companion remains in `A:\makerspace\smartwatch\companion`;
  `tools/watch_media_bridge.py` is also retained here.

Official sources, board pins and revision differences are in
[HARDWARE.md](docs/HARDWARE.md).
