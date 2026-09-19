# Live watch data

Updated 2026-09-15. Target: ESP32-S3-Touch-AMOLED-1.43-B, Ultra face 4.

## Implemented

- **Daily steps:** the existing QMI8658 cadence detector samples acceleration
  and gyro at about 50 Hz. Eight coherent steps start a walking bout and are
  credited together. Brief gestures and vigorous spinning are rejected. Counts
  and active time now persist in NVS once per minute when changed, and on
  orderly sleep. Sudden power loss can lose up to one minute. Local midnight
  resets the counters; an unknown clock defers rollover until synchronization.
  Wearing trials and stride calibration remain necessary. This is an estimate.
- **Activity:** steps, active cadence minutes and estimated distance drive the
  three rings (10,000 steps / 30 minutes / 5 km). Calories and distance have a
  visible estimate prefix. Invented stand-hours have been removed.
- **Battery:** Ultra and Control now honor the qualified ADC reading. Ultra
  shows `USB` on external power and `--` when uncertain. The Battery app shows
  system voltage and an approximate percentage only on inferred battery power.
  GPIO4 senses a shared system rail: USB readings cannot establish cell charge,
  remaining capacity or charging current. There is no fuel-gauge IC.
- **Brightness:** Settings shows a percentage. Settings, Control and USB `bri`
  use the same AMOLED brightness value, serialized through the display worker.
  The Ultra sun-marked dial now uses that brightness setting (previously it
  incorrectly displayed activity minutes), with a curved percentage label.
  Settings supports live dragging and saves once on release. USB changes
  persist too. Idle dimming/AOD still temporarily reduces the
  panel output; the saved setting returns on wake. No ambient-light sensor is
  fitted, so automatic ambient brightness is not available from this hardware.
- **Weather:** the watch itself fetches current temperature, feels-like,
  five two-hour forecast slots and four daily forecasts from
  [Open-Meteo](https://open-meteo.com/en/docs). HTTPS validates certificates
  using the ESP-IDF CA bundle; TLS buffers use PSRAM. It refreshes every 30
  minutes while connected, retries failures after a minute, and keeps network
  work off the UI and sensor tasks. Incomplete/malformed responses are rejected.
  Values older than one hour are marked stale; Ultra hides stale weather.
  Conditions are provider estimates for a location, not an on-watch air sensor.
- **Time:** saved Wi-Fi reconnects automatically and starts NTP. USB time sync
  is also supported. The RTC stores UTC and the watch displays the saved offset
  (currently IST, +330 minutes).
- **SpO2:** the dial tip is a rotated square. SpO2 and heart-rate values show
  `--` because this board has no optical pulse-oximetry sensor. The prior demo
  percentage and “Optimal” assessment have been removed.

The initial city is **Bengaluru**, inferred approximately from this PC's public
internet connection, not GPS. The coordinates are saved on the watch; they do
not follow travel automatically. A city change immediately invalidates the old
weather cache. No hotspot password is compiled into firmware or stored in the
source tree. Credentials are sent over USB and stored in the watch's existing
NVS settings; command logs redact the Wi-Fi payload.

## Setup and checks

```powershell
python tools/watchctl.py --port COM3 --wifi-ssid "Your hotspot"+# Password prompt, or WATCH_WIFI_PASSWORD in the process environment.
python tools/watchctl.py --port COM3 --sync-time
python tools/watchctl.py --port COM3 location 12.9719 77.5937 Bengaluru
python tools/watchctl.py --port COM3 weather
python tools/watchctl.py --port COM3 weather refresh
python tools/watchctl.py --port COM3 activity
python tools/watchctl.py --port COM3 power
python tools/watchctl.py --port COM3 bri 176
python tools/watchctl.py --port COM3 bri
```

Weather is app **6**, Activity **7**, Battery **21**, Settings **23**. Swipe or
tap Weather to switch current conditions / daily forecast; hold to refresh.
`weather` reports validity, age, observed timestamp, Wi-Fi and clock state.

The supplied Windows hotspot was off; it was started on **2.4 GHz** using
[Windows tethering APIs](https://learn.microsoft.com/en-us/uwp/api/windows.networking.networkoperators.networkoperatortetheringmanager).
`tools/hotspot.ps1` reads its status without displaying the password. Run using
Windows PowerShell 5.1. Its explicit `-Start` option is restricted to the
user-supplied SSID and preserves its credentials.

## Accelerometer / gyro proposal

No extra accelerometer or gyro is needed: the onboard **QMI8658** already
provides acceleration in g and angular speed in degrees/second. Diagnostics
(`diag imu`) and Level (`open 17`) expose them today. The configuration and
pin mapping follow the [Waveshare board documentation](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43).

Suggested next UI change, subject to the preferred layout:

| Place | Readout | Implementation |
|---|---|---|
| Optional center widget | Two-axis tilt / level dot | Low-pass the gravity vector; show pitch and roll, calibrated to the watch's reading pose. |
| Optional lower-left complication | Movement intensity | Remove the gravity baseline and graph acceleration magnitude over the last 10 seconds. Keep it separate from step count. |
| Detail page | Rotation speed + X/Y/Z traces | Gyro magnitude ring with numeric degrees/second; a tap switches acceleration and gyro graphs. |

Use the existing timestamped sensor cache; sample processing stays at 50 Hz,
but redraw widgets at 10 Hz and retain a short fixed-size history. Freeze the
snapshot once per display frame to avoid stripe inconsistencies. Estimate gyro
bias while still and stop showing live motion when samples become stale.

A gyro alone cannot provide a stable north heading: integrated yaw drifts.
Keep the current compass rose decorative, or add a magnetometer if actual north
is required. Raise-to-wake already uses this IMU; additional motion widgets
should share its samples rather than increase I2C polling.

## Validation artifacts

Device verification completed on COM3 at 14:38 IST: Wi-Fi and NTP were up,
HTTPS returned Bengaluru **31 C / feels 34 C**, the RTC was valid, and the IMU
was valid with zero steps while resting. A USB reset rejoined the saved hotspot
and fetched weather without reprovisioning. Brightness **176** survived reset;
the previous setting **175/255 (69%)** was restored. Ultra face 4 remains open.
The initial live-data application SHA-256 was
`9ccb9b89acd1f1dbe3ca56c068f9890977a00bcdcf119f48b5f72bdc3d44708e`.

- `build/live-data-tests.log`: host regression tests, including daily rollover,
  reboot restoration, weather response validation and cached Ultra rendering.
- `tests/fixtures/weather.json`: fixed Open-Meteo response for offline parser
  tests, including the half-hour timezone case.
- `build/live-data-flash.log`: firmware build, USB flash and segment hashes.
- `build/live-data-network.log`: association, DHCP, NTP, TLS certificate
  validation and the first successful Bengaluru weather fetch.
- `build/live-data-reboot.log`, `build/live-data-usb.jsonl`: restart and USB checks.
- `build/live-data-preview/` and `build/live-data-shots/`: host-rendered visual
  checks. They use explicitly synthetic simulator weather.

Physical brightness luminance, battery discharge calibration and worn step
accuracy have not been measured. Device reads establish sensor availability;
host screenshots establish rendering, not physical panel appearance.

### Brightness follow-up

The pre-fix USB reproduction showed a Settings drag from `(160,163)` to
`(340,163)` leaving brightness at 122; a tap at the destination changed it to
240. The fix handles raw drag samples and captures only touches beginning on
the slider. The Ultra dial now invalidates its cached artwork whenever the
brightness setting changes. Brightness tests cover dial updates, live drag,
tap, endpoint clamping, unrelated touches and Control Center using the same
value. Run `build/sim.exe --brightness-checks` from an isolated output directory.

`bri` also reports `panel_command`, the last successful brightness command sent
to the AMOLED. It is useful for checking idle dimming and the display queue;
it is not a sensor measurement of screen luminance. Validation logs and preview
images for this correction are under `build/brightness-*`.

The correction was flashed to COM3 and its application SHA-256 is
`7188cc7cc3aeb0bb07f05fd119bb638394abf53ed422f55688c28855b76919eb`.
Hardware USB checks confirmed dragging Settings in either direction changes
both the saved level and the panel command. The user's pre-test brightness
setting **122/255 (48%)** was restored and Ultra left open.
