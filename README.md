# ModularWearableOS — NOS v0.5 Alpha

A from-scratch smartwatch OS for the **Waveshare ESP32-S3-Touch-AMOLED-1.43-B**
(466×466 round AMOLED, capacitive touch, one side button, 16 MB flash, 8 MB PSRAM).
NOS is Nothing-inspired: true black, white ink, dot-matrix numerals, caps mono
labels and at most one red. It targets 40 fps with a 30 fps floor.

![Every screen](docs/assets/nos/showcase.png)

| Pager | Control centre | Notifications | Launcher + iris |
|---|---|---|---|
| ![](docs/assets/nos/g_pager.gif) | ![](docs/assets/nos/g_control.gif) | ![](docs/assets/nos/g_notifs.gif) | ![](docs/assets/nos/g_iris.gif) |

**New here? Start with the [Getting Started guide](docs/GETTING_STARTED.md).**

## Features

- **9 watch faces:** DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE and
  **Solar/Orrery**. Orrery is a live solar-system face with real ephemerides and a
  hold-to-tilt and time-travel mode. Every face has an always-on version.
- **Shell:** a home pager with widgets, an integrated Apps page, a paged Control
  Center, and notifications (banner, list and detail). The launcher opens apps with
  an iris animation; double-pressing the button reopens the last app.
- **Health and time:** Activity, Heart rate, Breathe, Sports workouts, Energy,
  Timer, Stopwatch, Alarms, World clock.
- **Outdoor:** Compass, Barometer, Navigate, Weather (Open-Meteo over Wi-Fi).
- **Connect:** Music control, Find phone, Torch, Call. The phone link uses
  [Gadgetbridge](https://gadgetbridge.org/), where the watch appears as a Bangle.js.
- **System:** Settings, Modules, Battery history, Sensor logs, the IMU app,
  Developer tools, and a guided **Diagnostics** test of every board part.
- **Storage:** settings, sports journals and sensor logs are kept on the microSD
  card. Saves are checksummed, double-buffered and read back after writing.
- **Desktop simulator:** the full OS runs on Windows for development and
  scripted tests.

## Repository layout

| Path | Contents |
|---|---|
| `nos/` | The OS: engine, shell, apps (`nos/apps/`) and faces (`nos/faces/`) |
| `firmware/` | ESP-IDF project, board drivers, pin map (`firmware/main/pins.h`), partitions |
| `sim/` | Windows simulator for NOS |
| `core/` | Shared graphics and UI core |
| `tools/` | Build, flash, packaging, USB control and validation scripts |
| `tests/` | Host and simulator tests |
| `docs/` | Design spec, hardware notes and engineering records |

## Quick start

```powershell
# Simulator (needs GCC/WinLibs)
.\sim\nos_build.ps1 -Strict -Run

# Firmware (needs ESP-IDF 5.5.x)
.\tools\build.ps1
.\tools\build.ps1 -Port COM3 -Flash
```

See [Getting Started](docs/GETTING_STARTED.md) for prerequisites, the microSD
card, first boot, Wi-Fi and phone pairing.

## Hardware notes and limits

- The 1.43-B has **no microphone, speaker or vibration motor**. Alerts are visual,
  and the Recorder app shows a microphone-needed screen.
- The battery reading measures the system rail, so the percentage is an estimate,
  not a fuel gauge.
- No tearing-effect (TE) line is routed from the panel, so frame pacing is software only.
- The **1.43C** board is a separate target with different wiring (`tools/build_c143.ps1`).
  Never flash one board's build onto the other.

## Documentation

- [Getting Started](docs/GETTING_STARTED.md): set up, build, flash and first boot
- [NOS overview](docs/NOS.md): screens, performance history, device lessons
- [Design spec](docs/NOS_DESIGN.md): visual, motion and wording rules
- [Hardware](docs/HARDWARE.md): board pins and revisions
- [Simulator](docs/NOS_SIM.md): headless scripts, recording, damage checks
- [Host validation](docs/HOST_VALIDATION.md): test profiles and CI
- [Orrery](docs/ORRERY.md), [Sports](docs/SPORTS_UPDATE.md),
  [Motion wake](docs/MOTION_WAKE.md), [Battery](docs/BATTERY_FIT.md)

## Branches

`main` is the current NOS line. The earlier firmware, from before NOS (the WOS
runtime and display-optimisation work), is preserved on the `legacy` branch.
