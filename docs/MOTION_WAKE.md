# IMU readings and motion wake

Implemented for the ESP32-S3-Touch-AMOLED-1.43-B on 2026-09-13. Left wrist is
the default. This wakes the automatically dimmed/off display and opens a
10-second full-brightness glance from Always On Display (AOD). Explicit
**Sleep / Power Off** still uses deep sleep and requires BOOT to wake.

## Why aZ is about -1 while still

The accelerometer contains a tiny suspended mass. It senses specific force,
including the effect of the watch being supported against gravity. This
firmware reports that in **g**, where 1 g is about 9.81 m/s². At rest, the
length of the three-axis vector is normally about one, not zero:

`sqrt(aX*aX + aY*aY + aZ*aZ) ≈ 1 g`

The live watch before this update reported approximately
`(0.065, 0.011, -0.996) g`: normal for its current orientation. In its current
face-up mounting Z is negative. Turn it over and Z should become positive;
hold it on an edge and most of that reading moves to X or Y. The screen's
90-degree graphics correction does not rotate the raw sensor axes.

The gyroscope measures **angular speed**, in degrees per second, around each
axis. It does not directly measure an angle. At rest it should be near zero,
with some noise and offset; this unit showed roughly `(4, 3, -0.5) °/s`.
The wake detector estimates this offset after 1.5 seconds of steady samples.
Diagnostics continues to show the raw data so the correction is not hidden.

To estimate motion-only acceleration, first estimate gravity's direction and
subtract its vector. Simply subtracting 1 from Z only works in one fixed
orientation. The gyro helps detect a turn; the accelerometer helps establish
the final tilt. Neither provides reliable absolute compass heading on its
own, and this wake detector does not estimate pickup height.

Reference: [Analog Devices AN-1057: Using an Accelerometer for Inclination
Sensing](https://www.analog.com/en/resources/app-notes/an-1057.html).

## Set it up

1. Open **Settings → Wake gestures**. **Left** and both gesture switches are
   enabled on the installed watch.
2. Hold your wrist in the position in which you naturally read the screen.
3. Tap **Learn view**, then keep that position still for about one second.
   Wait for **Your viewing angle saved**. Movement is allowed for up to ten
   seconds while finding the pose; it must then be held steadily.
4. Lower the wrist, let the screen dim/off, and lift/turn it to read. From a
   desk, let it rest first, then lift it and hold it facing you.

Both wrists support either direction of rotation. Choosing a wrist selects
its own saved reading pose; changing wrists does not erase the other pose.
Until learned, both use a face-up reading direction. Gravity alone cannot
distinguish your left wrist from your right wrist or which way you are facing.

The normal idle timeout remains 30 seconds to dim and 120 seconds to turn
the display off. A visible Pip greets a detected wake; care rewards retain
their existing rate limits. AOD returns to its dim, one-Hz display after a
ten-second glance. Touch remains a fallback.

## How detection works

`motion_wake.c` is a bounded state machine; `wake_service.c` owns settings,
fresh-sample handling and pose learning on the main UI task.

- Uses cached QMI8658 samples with their acquisition timestamps. A sample is
  counted once. The hardware service rejects data older than 150 ms; a
  detector gap over 200 ms cancels the gesture.
- Filters the gravity direction with an 80 ms time constant. Raw values
  remain available to Diagnostics, Pip and walking estimation.
- Pickup arms after 500 ms at rest. Wrist turn arms after 250 ms pointing
  more than 60 degrees away from the reading direction.
- Sustained angular speed above 22 °/s or acceleration magnitude deviating
  from 1 g by more than 0.12 g begins a candidate movement.
- A wrist turn needs at least 20 degrees of integrated rotation and about
  25 degrees of change in gravity direction. Pickup needs at least 12 degrees
  of rotation, or sustained linear-acceleration evidence.
- Both finish inside a 45-degree cone around the selected reading direction,
  with motion settling for 160 ms. Candidates expire after 1.8 seconds.
- Strong impacts, free fall, repeated reversals and excessive rotation cancel
  a candidate. A two-second cooldown prevents immediate repeated wakes.
- Motion wake is inactive while the screen is fully active, while learning a
  pose, when both switches are off, or while `awake 1` is in use.

These are initial thresholds tested with synthetic traces. Real walking,
typing, driving, bed movement, different straps and wearing angles still need
owner traces. Shakes in the test set are rejected; that does not establish
zero false wakes in daily use.

## Persistence and power

Preferences and two normalized poses are stored in a versioned, checksummed
48-byte NVS record under `wake_cfg`. Only user changes/learning write flash.
Failed writes leave the previous live configuration in place. The old
settings record is retained and its tilt switch supplies the initial enabled
state when no valid wake record exists.

This release uses software polling of the already-running board service.
It does **not** implement IMU interrupt wake from ESP32 deep sleep. The panel
turning off is not equivalent to low-power MCU standby. Battery fit, charging
and a 24-hour budget are covered in [BATTERY_FIT.md](BATTERY_FIT.md).

## USB diagnosis and reproducible checks

Use `python tools/watchctl.py --port COM3` followed by one of:

| Command | Purpose |
|---|---|
| `imu` | Raw acceleration in g and gyro in °/s |
| `diag imu` | Open the IMU diagnostics page, labelled with gravity units |
| `wake` | Profile, modes, learned status, state, count, last reason, motion and bias |
| `wake left` / `wake right` | Select and save a wrist |
| `wake pickup 0` / `wake pickup 1` | Disable/enable pickup |
| `wake turn 0` / `wake turn 1` | Disable/enable wrist turn |
| `wake learn` | Start learning the currently selected wrist's reading pose |
| `screen off` / `screen dim` | Exercise normal display wake without waiting for timeout |
| `screen` | Read display and AOD/glance state |

Read-only `wake`, `screen`, `imu`, `board`, `info`, `time`, `activity`,
`power`, `pip` and `face` queries do not count as display activity. This makes
USB observations possible without the observer itself waking the watch.

- `tools/test.ps1`: detector traces at 25/50 Hz, mirrored wrists, pickup/turn
  separation, settle/cooldown, bumps/shakes/falls, invalid/stale/duplicate data,
  gyro bias, learned tilt, disabled modes, clock wrap, persistence, failed
  saves and independent pose learning. Results: `build/test-wake-final.log`.
- `sim.exe --wake-checks` in an isolated working directory exercises the real
  settings touch targets and produces round-panel previews. Results and
  inspected screenshots: `build/wake-preview/`.
- `tools/wake_device_check.py` checks hardware USB/settings/power behavior. It
  temporarily selects right wrist with pickup disabled for a reboot check.
  After reboot, `--after-reset` verifies retention and restores left wrist
  with both modes enabled. It does not simulate or prove physical gestures.
- The exact installed image hashes and completed hardware observations are
  recorded in [device-validation.json](device-validation.json). Wear testing
  and battery runtime remain separate pending checks.

The installed application SHA-256 is
`0cae57a370e1fd76b257e0371f3bbceb350b497508af7a4ac320cc6b2a5421b2`.
Hardware checks passed for the settings touch targets, independent gesture
switches, left/right NVS retention, live IMU, stable off/dim behavior while
queried, touch wake and the ten-second AOD glance. The complete repeat of the
five-expression benchmark measured 60 FPS in every sampled window. One
earlier USB acknowledgement timeout is recorded with the validation evidence.
No physical pickup or wrist-turn success is claimed by these automated checks.
