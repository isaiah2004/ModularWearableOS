# NOS v0.5 Alpha — Waveshare 1.43-B

The active runtime is NOS on branch `NOS-v0.5-Alpha`, including Solar/Orrery,
Developer tools, the integrated home/apps pager, Control Center paging and Sports.
Build the recovered **current workspace**, including its dirty and untracked
sources. Git HEAD predates this work. Preserve the recovery archives and
`build/alpha05/source-baseline.zip` / `source-baseline.json`; see
[AGENTS.md](AGENTS.md) and [recovery provenance](docs/RECOVERY_20261020.json).

This target is the ESP32-S3-Touch-AMOLED-1.43-B (466×466, 16 MB flash,
8 MB octal PSRAM). The 1.43C has different wiring and a separate legacy build.
NOS rendering/power work targets 40 fps with a 30 fps floor; old measurements
below describe their recorded firmware, not validation of the current binary.
See [NOS](docs/NOS.md), [rendering work](docs/OPTIMIZATIONS.md),
[Sports](docs/SPORTS_UPDATE.md), and [host validation](docs/HOST_VALIDATION.md).

## Build and flash

From this directory in PowerShell, using ESP-IDF 5.5.x:

```powershell
.\tools\build.ps1
.\tools\build.ps1 -Port COM3 -Flash -Monitor
```

`-IdfPath` selects another ESP-IDF installation (default `A:\makerspace\esp-idf`).
The wrapper configures, snapshots the actual source/config/assets, builds, and
checks linked features, ELF/image identity and app-slot headroom. A source change
during compilation stops publication. Nothing is built from a clean HEAD worktree.
`TEMP`/`TMP` and private run directories stay under `.worktrees/_audit_tmp` on the
workspace volume; existing directories are preserved.

For an existing **identity-verified build**, use the ESP-IDF Python environment:

```powershell
python tools/flash_watch.py --port COM3 --dry-run
python tools/flash_watch.py --port COM3
.\tools\package.ps1
```

Packaging creates a new `dist/NOS-v0.5-Alpha-<bundle-id>/` containing exact source,
assets, config/dependency lock, ELF/map, toolchain identity and checked updater.
Its `flash.ps1 -Port COM3 -Python <ESP-IDF-python.exe>` uses the same preflight and
checkpoint path. Offsets come from the generated flash plan, including OTA data;
older fixed-offset packages are historical. Never flash a merged image for an
update because gap bytes overwrite NVS. No erase-all operation is needed.

Before any reset/write, both routes require a completed preference checkpoint
with `ok=1 stored=1 match=1` and verified flash ranges excluding NVS. Settings are
authoritative on SD at `/watch/settings.0.bin` and `/watch/settings.1.bin`, loaded
into RAM at boot. Their alternating checksums, fsync/readback checks, mount lease
and SD lock are required; failed saves remain dirty for retry. NVS is only the
migration/card-absent fallback. See [settings evidence](docs/SD_SETTINGS_20261020.json).
Close serial monitors and use a USB data cable. Hardware evidence in a bundle
applies only when its segment hashes match; physical checks remain separate.

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
- **Sensor logs** records raw IMU segments at `/imu/NNNN.csv`. Sports journals
  live under `/watch/sports/`, workout checkpoints under `/watch/workouts/`, and
  Recorder WAV files under `/watch/memos/` on hardware with a microphone.
- Check capacity/mount status with `sd` or Settings modules. Wait for recordings
  and preference saves to finish, then fully power off the board before removing
  or reinserting the card. Display sleep alone is insufficient. NOS does not
  expose the legacy WOS `sdtest`, `sdeject` or `sdmount` commands; automatic
  hot-plug is not implemented.

[Card specifications](https://www.sandisk.com/products/memory-cards/microsd-cards/sandisk-high-endurance-uhs-i-microsd?capacity=32GB)
and [SDHC/FAT32 guidance](https://support-en.sandisk.com/app/answers/detailweb/a_id/9063).

## USB control and first checks

```powershell
python -m pip install pyserial
python tools/watchctl.py --list
python tools/watchctl.py --port COM7 info
python tools/watchctl.py --port COM7 state
python tools/watchctl.py --port COM7 sd
python tools/watchctl.py --port COM7 runtime_state
python tools/watchctl.py --port COM7 prefs
python tools/watchctl.py --port COM7 --smoke --log build/usb-smoke.jsonl
```

The NOS smoke sequence requires an already unlocked, active watch with no active
or unresolved recording owners and matching saved preferences. It checks status,
opens Energy, Battery history, Sports and Developer tools by name, and finishes
at HOME. It preserves the awake flag and preferences. Its timestamped log can be
paired with a camera recording; it does not verify physical screen appearance.
See [current host validation](docs/HOST_VALIDATION.md). The older
[USB command reference](docs/USB_CONTROL.md) describes legacy WOS commands.
`--sync-time` and `--wifi-ssid` retain that explicitly detected legacy runtime's
protocol and are refused on NOS before any password prompt or mutation.

RTC time is stored in **UTC**, with a saved display timezone (default IST).
An unset/stopped RTC is reported as unsynchronised. For NOS, use Settings,
phone/Gadgetbridge synchronization, or Wi-Fi/NTP to set it. RTC retention needs power from
the board or its RTC backup supply; it cannot retain time without either.

## Simulator and tests

```powershell
.\sim\nos_build.ps1 -Strict -Verify
.\sim\nos_build.ps1 -Strict -Run
python tools/validate.py --profile host
python tools/validate.py --profile offline --sim build/nos_sim.exe
```

Every simulator build is strict. Its content manifest records compiler/flags,
source/header and object hashes; failed compile/link/verification never publishes
a successful new executable. `-Verify` compares the same current sources with and
without cost hooks. Named outputs have separate locked caches. See
[NOS simulator](docs/NOS_SIM.md) and the [validation matrix](tools/validation_matrix.json).
The portable host profile runs host/protocol/build regressions; the offline
profile adds all current Python modules, native cases, script integrity and Sports
acceptance using the supplied strict simulator. Reports list every skip explicitly.

`sim/build.ps1`, `build/sim.exe` and `tools/test.ps1` retain historical core/apps
coverage. They do not validate the current NOS runtime. Simulator sensor values
are synthetic and do not establish device power/timing or sensor accuracy.

The following Pip/prototype material is historical core/apps documentation.
Current NOS behavior and commands are documented in the linked NOS/Sports guides.

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
