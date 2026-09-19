# USB control

Native ESP32-S3 USB Serial/JTAG, 115200 baud (USB ignores physical baud).
Close `idf.py monitor` before running `watchctl.py`. One command per line;
send the next command only after a line exactly equal to **DONE**. Background
ESP-IDF log lines can appear between response lines. Commands run on the UI
task; the USB reader only enqueues them. An injected gesture completes before
the next queued command is executed.

| Command | Result |
|---|---|
| `help`, `apps`, `info` | Command help, all app IDs, current app |
| `diag touch` | Open Diagnostics touch targets, live crosshair, coordinates, and tap counter; BOOT/home exits |
| `diag imu` | Open raw acceleration/gyro Diagnostics; acceleration includes gravity |
| `board` | Board, PSRAM, I2C devices, RTC validity, SD, system voltage, and internal heap free/largest/minimum bytes |
| `imu` | Cached acceleration in g and gyro in degrees/s; valid flag |
| `sd` | Mount status, capacity/free bytes and most recent storage result |
| `sdmount` | Mount/retry; never formats |
| `sdtest` | Unique temporary-file write/read/compare/remove test |
| `sdeject` | Wait for logging, unmount; wait for ESP_OK before removal |
| `tap X Y` | Tap using upright UI coordinates, each in 0..465; physical touch is transformed separately |
| `swipe X0 Y0 X1 Y1` | About 240 ms synthetic gesture |
| `left`, `right`, `top`, `bottom` | Open the corresponding edge overlay |
| `close`, `home`, `open N` | Close overlay, watch face, or app ID |
| `face N` | Select face 0..12 and save preference |
| `bri`, `bri N` | Read setting and last successful panel command / set and persist AMOLED brightness 1..255 |
| `time` | Current displayed time and UTC offset |
| `tz N` | Set/persist offset in minutes east of UTC; IST = 330 |
| `settime Y M D h m s` | Set local calendar time; updates RTC in UTC |
| `awake 1`, `awake 0` | Disable/restore idle timers and motion wake |
| `wake` | Read wrist, gesture switches, learned pose status, detector state, wake count/reason, gravity magnitude and gyro bias |
| `wake left`, `wake right` | Select/persist wrist and its independent reading pose |
| `wake pickup N`, `wake turn N` | Disable/enable each gesture independently; N is 0 or 1 |
| `wake learn` | Learn the selected wrist's reading pose; hold still for about one second |
| `screen off`, `screen dim` | Enter display off/dim immediately with motion and touch wake available |
| `screen` | Read off/dim/awake state plus AOD and temporary glance flags |
| `prof 0`, `prof 1` | Disable/enable USB frame telemetry (off on boot) |
| `perf display info`, `perf display probe` | Display configuration and cached raw controller probe evidence |
| `perf display app NAME SEC` | Timed app trace; acknowledge now, emit PERF_APP after SEC |
| `perf display fill N SOURCE`, `perf display rect W H N [SOURCE]` | Synthetic transfer test; SOURCE is sram, generator or psram |
| `perf display sweep`, `perf display status`, `perf display export` | Fixed-clock buffer/source sweep, counters, last four app traces |
| `aod` | Toggle dim watchface display; not deep sleep |
| `sleep` | Deep sleep; BOOT wakes, USB disconnects (no DONE expected) |
| `pause`, `resume` | Pause/resume rendering; USB remains responsive |
| `bleinfo`, `bledrop` | BLE status / drop connections and readvertise |
| `wifi SSID PASSWORD` | Join/save WPA network; simple credentials without whitespace |
| `wifi {"ssid":"...","password":"..."}` | Join/save WPA network including spaces; prefer `watchctl.py --wifi-ssid` and its password prompt |
| `location LAT LON CITY` | Save weather coordinates and city (ASCII, 1..31 bytes) |
| `weather`, `weather refresh` | Read cached weather/link status / request background refresh and time sync |
| `hud`, `pip`, `battdump` | Existing renderer, pet, and battery diagnostics |

Apps 0..23 keep their previous IDs; **24 is Storage**, **25 Pet care**, **16 Diagnostics**,
**17 Level**. Recorder 10 and Assistant 11 show the no-microphone state.

Use `tools/display_perf.py` for asynchronous performance traces and long sweeps,
then `tools/display_perf_report.py` for Markdown/JSON analysis. Synthetic tests
temporarily occupy the UI task; touch/commands resume after completion. See
[display baseline and metric definitions](display_baseline.md).

`watchctl.py --sync-time` copies this PC's local date/time and offset. Use
`--script commands.txt --log build/session.jsonl` for a repeatable sequence.
Commands and timestamped responses are local files; nothing is published to
an external service. Wi-Fi credentials are redacted from command log entries.

`--smoke` runs board/IMU/SD status, all overlays, Diagnostics, Level, Storage,
and returns home. A DONE confirms command completion, not physical correctness;
read the sensor valid flags and inspect the screen with the later camera.
For card validation run `sdtest` separately and require **SDTEST ESP_OK**.

Weather setup, daily steps, qualified battery readings, brightness persistence
and the accelerometer/gyro proposal are in [Live data](LIVE_DATA.md).
Weather is fetched on the watch over HTTPS. Current location is saved, not GPS.

Capture a fresh boot, including messages before the command interface starts:

```powershell
python tools/usb_capture.py --port COM3 --reset --seconds 20 --output build/boot.log
```

This resets the processor without modifying flash. Omit `--reset` to capture
the current runtime. The tool reconnects if native USB re-enumerates; close
it before opening `watchctl.py` or another serial monitor.

Pip's commands are `pip`, `pip care`, `pip hello`, `pip pet`, `pip play`,
`pip routine WAKE REST`, `pip center`, `pip save`, `pip live`, and
`pip demo ACTION`. `power` exposes qualified power evidence, `activity` the
cadence estimate, and `notify APP|TITLE` injects an incoming notification.
See [Living with Pip](PIP_LIFE.md) for the interaction and transport contracts.

The reproducible device tour is `python tools/pip_device_check.py --port COM3`.
It requires the confirmed watch identity and temporarily changes the care
routine for daytime notification testing, restoring it at the end. Its
current test window assumes the local hour is before 23:00. App checks poll
for transition completion; a command acknowledgement alone can precede the
new app becoming current.

Motion wake setup, sensor units and thresholds are documented in
[MOTION_WAKE.md](MOTION_WAKE.md). Read-only `wake`, `screen`, `imu`, `board`,
`info`, `time`, `activity`, `power`, `pip`, and `face` queries do not wake the
display or extend its idle timeout. `prof 0`/`prof 1` likewise do not count as
activity. Explicit sleep still needs BOOT; display off is not deep sleep.
