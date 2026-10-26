# Getting started with NOS

This guide takes you from a fresh clone to NOS running on a Waveshare
ESP32-S3-Touch-AMOLED-1.43-B. You can also try it in the simulator first, without
any hardware.

## 1. What you need

**Hardware**

- A Waveshare **ESP32-S3-Touch-AMOLED-1.43-B** board. The 1.43C is a different
  board with its own build; see [Other boards](#other-boards).
- A USB-C **data** cable. Charge-only cables will not enumerate.
- A microSD card. Recommended: a 16–32 GB microSDHC formatted **FAT32**, with one
  partition. Larger cards work once reformatted to FAT32; exFAT is not supported.
- Optional: an Android phone with Gadgetbridge, for notifications, time and music.

**Software** (Windows and PowerShell are the supported host)

| For | Install |
|---|---|
| Firmware | [ESP-IDF **5.5.x**](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/windows-setup.html) |
| Simulator and tests | GCC for Windows ([WinLibs](https://winlibs.com/)) and Python 3.12 |
| USB tools and tests | `python -m pip install -r tools/requirements-host.txt` |

Keep two Python environments apart: ESP-IDF's own environment for building and
flashing, and a normal Python 3.12 for the simulator, tests and USB tools. Don't
install a separate esptool into the ESP-IDF environment.

## 2. Get the source

```powershell
git clone https://github.com/isaiah2004/ModularWearableOS.git
cd ModularWearableOS
```

Local secrets go in `.env`, which git ignores. Copy `.env.example` to `.env` only if
you use the optional desktop tools that need it. Never commit `.env`.

## 3. Try the simulator (no hardware)

```powershell
.\sim\nos_build.ps1 -Strict -Run
```

This builds `build/nos_sim.exe` and opens a 466×466 window running the same NOS
code as the watch. The mouse is your finger: click to tap, and drag to swipe.
Pass `-Compiler <path-to-gcc.exe>` if GCC is not on your `PATH`. For headless
scripts, screenshots and recordings, see [NOS_SIM.md](NOS_SIM.md).

## 4. Build the firmware

The scripts expect ESP-IDF at `A:\makerspace\esp-idf`. Point them elsewhere with
`-IdfPath`:

```powershell
.\tools\build.ps1                                # default ESP-IDF location
.\tools\build.ps1 -IdfPath C:\Espressif\esp-idf  # or your own install
```

The build script configures, builds and records a build identity, which is a
snapshot of the source, config and assets. It also checks that the image includes
the core features (Solar/Orrery, the home pager, Sports and Developer tools) and
fits its app slot. Output goes to `firmware/build/`.

## 5. Flash the watch

1. Close any serial monitor, then plug the watch in.
2. Find its COM port in Device Manager under *Ports (COM & LPT)*. It enumerates as
   a native ESP32-S3 USB device (`303A:1001`).
3. Do a dry run, then flash:

```powershell
python tools/flash_watch.py --port COM3 --dry-run    # ESP-IDF Python
.\tools\build.ps1 -Port COM3 -Flash
```

The flash is **checked**. Before writing, it asks the running firmware to
checkpoint your preferences, and it refuses to write any range that overlaps
NVS. Your settings and pairing survive updates.

- **First flash** onto a board with other firmware: the checkpoint cannot run,
  because that firmware does not answer NOS queries. Use ESP-IDF's
  `idf.py -C firmware -p COM3 flash`. It is safe on a blank or factory board.
- **Watch not answering over USB:** wake it with the side button, or replug it.
  If it is frozen, hold BOOT while you reset it to enter download mode.

Never flash a merged full-flash image as an update: the gap bytes overwrite NVS.

## 6. microSD card

Insert the card with the board **powered off**, then boot. The watch mounts it at
`/sdcard`. It never formats a card, and it boots normally without one.

| Path | Contents |
|---|---|
| `/watch/settings.0.bin`, `settings.1.bin` | Settings (two checksummed copies) |
| `/watch/sports/`, `/watch/workouts/` | Sports journals and workout checkpoints |
| `/imu/NNNN.csv` | Sensor log recordings |

Without a card, settings fall back to internal flash (NVS). Wait for recordings
and saves to finish, then **fully power off** before removing the card. Display
sleep is not enough, because there is no hot-plug support.

## 7. First boot

- **Wake:** press the side button, or tap the screen.
- **Home:** swipe left and right through the faces, widgets and Apps. Swipe down
  for Control Center, and up for notifications.
- **Change face:** touch and hold the face to open the picker.
- **Orrery:** hold the face to tilt the view, keep holding to unlock time travel,
  and hold longer still to open the face picker.
- **Back:** press the side button. Double-press reopens the last app.
- **Check the hardware:** open *Diagnostics* and run the guided test. Every result
  is also printed over USB as `DIAG ...`.

## 8. Set the time, Wi-Fi and phone

The RTC stores **UTC**; the display timezone is a setting (default IST). An unset
clock shows as unsynchronised. Any one of the following sets it:

- **Phone:** install Gadgetbridge and add the watch, which appears as
  **Bangle.js NOS**. Choose **not** to bond. Gadgetbridge then sets the time,
  forwards notifications and controls music. See [PHONE.md](PHONE.md) for the full steps.
- **Wi-Fi:** in *Settings → Connectivity*, turn Wi-Fi on and choose a network, or
  use *Phone setup* to join a hotspot. Once online, the watch syncs time over NTP
  and fetches weather from Open-Meteo.

## 9. Talk to it over USB

```powershell
python tools/watchctl.py --list
python tools/watchctl.py --port COM3 info
python tools/watchctl.py --port COM3 state
python tools/watchctl.py --port COM3 sd
python tools/watchctl.py --port COM3 --smoke --log build/usb-smoke.jsonl
```

The smoke test opens several apps by name and returns to the home screen. Run it
with the watch awake and unlocked. The watch also accepts input commands such as
`tap`, `swipe`, `btn`, `open NAME` and `face N`, which the bench scripts use.

## 10. Run the tests

```powershell
python tools/validate.py --profile host
.\sim\nos_build.ps1 -Strict -Verify
python tools/validate.py --profile offline --sim build/nos_sim.exe
```

The host profile runs protocol and build checks. The offline profile adds the
simulator-driven cases. Each report lists every skipped test explicitly.
CI runs the same profiles from `.github/workflows/host-validation.yml`. Details
are in [HOST_VALIDATION.md](HOST_VALIDATION.md).

## Other boards

The **1.43C** board has different LCD, touch and audio wiring, and no SD card,
IMU or RTC. It builds separately:

```powershell
.\tools\build_c143.ps1              # build only
.\tools\build_c143.ps1 -Port COM4   # build and flash
```

Never flash a B build onto a C board, or the reverse. The C build must never
drive GPIO42 as an LCD enable, because GPIO42 is the C board's microphone input.
Pin maps are in `firmware/main/pins.h` and [HARDWARE.md](HARDWARE.md).

## Where next

- [NOS overview](NOS.md): every screen, and the device lessons
- [Design spec](NOS_DESIGN.md): the rules each screen follows
- [Orrery](ORRERY.md): the solar-system face
