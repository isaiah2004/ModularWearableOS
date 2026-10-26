# IMU

Open **IMU** in the launcher (`open imu` in the simulator). Bottom tabs:

- **Live:** acceleration and gyro XYZ traces, current values, acceleration magnitude,
  and an XY tilt bubble. X is white, Y medium grey, Z dark grey. Fixed scales are
  +/-2 g and +/-120 deg/s; out-of-range traces clip, numbers retain their values.
  64 display samples cover about 4.3 seconds at the normal 30 fps cadence. The live
  board view uses the existing filtered HAL readings; CSV contains raw register readings.
- **Record:** choose 25, 50, or 100 Hz, then START RECORDING. STOP + SAVE drains and
  closes asynchronously. The final counters remain visible; NEW RECORDING returns
  to rate selection. Samples count formatted rows, including the pending batch;
  bytes count bytes actually handed to the filesystem. DROP is ring overflow,
  MISS is skipped timer deadlines, ERR is failed I2C reads. Errors are shown in place
  of the filename on the final summary. A small REC dot appears in the IMU app, and the
  existing home/control recording chip opens the IMU app (microphone recording has priority).
- **Files:** highest-numbered recording first, `<` lower, `>` higher, duration and byte size.
  DELETE requires a second CONFIRM tap. Browsing/deletion is disabled during recording.
  All 10,000 numbered names are reachable; there is no fixed-size directory cache.

## Acquisition and sleep

`core/hal.h` adds `hal_imu_stream_start(hz)`, `stop`, `read`, and `get`. A point
contains monotonic milliseconds since start, UTC milliseconds, and six raw floats.
There is one consumer: the SD worker. Stop preserves queued points for draining;
start resets the queue and loss counters. Do not use the stream concurrently with
the recorder. `hal_imu_record_start/stop/get` and `hal_imu_files_request`,
`hal_imu_file_delete/get` provide nonblocking commands and cached UI snapshots.

`firmware/main/imu_hal.c` uses an `esp_timer` only to notify a dedicated sampler
task. It reads QMI8658 registers through `board_imu_raw`, timestamping immediately
before I2C. It skips missed deadlines instead of fabricating catch-up samples.
The existing board task, filter, knock/steps and raise-to-wake paths are unchanged.
The chip retains its existing 500 Hz ODR and hardware filters (including ~13 Hz
gyro filtering); a 100 Hz CSV is not a promise of 50 Hz gyro bandwidth or FIFO capture.

A second, lower-priority task drains the PSRAM ring, formats CSV, batches writes
up to 4 KiB and flushes at least once a second when SD is responsive. Filesystem
operations never run in tick/draw or a timer callback. Both tasks block when idle.
**Display sleep and AOD do not suspend either task.** Leaving the IMU app also keeps
recording. Restart/deep power-off and SD eject are refused while a recording or
file operation is active; stop and wait for completion first. This does not make
recording survive battery loss, forced reset, or physically removing the card.

The sim uses the same queue/writer with a deterministic deadline scheduler, not
one sample per frame. Synthetic gyro and gravity change at each sample's own
timestamp. Virtual SD operations run before the measured frame, synchronously on
the host; this proves cadence and UI behavior, not real SD latency or task scheduling.

## CSV and retrieval

Files are `/sdcard/imu/0000.csv` through `9999.csv`; the first unused name is
created with `O_EXCL`, so existing files are never overwritten. Deleted numbers
may be reused. The sim defaults to `build/sd/imu`; `NOS_SD_ROOT` selects an isolated
card root for tests (the root must already exist).

```csv
ms,utc_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,a_g
10,1791693570010,0.10000,0.00000,-1.00000,0.000,1.000,0.000,1.00499
```

`ms` is actual acquisition-start time relative to stream start, not a row number.
`utc_ms` is Unix UTC milliseconds, or 0 when the clock is unset; clock sync can
change UTC during a file, while monotonic elapsed time continues. Acceleration is
in g, gyro in degrees/second, `a_g = sqrt(ax_g² + ay_g² + az_g²)`.
Timing gaps expose losses; counters are session state, not extra CSV rows.
Files-page duration is the last complete sample's `ms`, so trailing losses do not
inflate it. The browser reads only the final 4 KiB and ignores an incomplete last
line. A failed write can leave a partial final row; import only complete rows.

Stop and save before downloading. List with the existing tool:

```powershell
python tools/sdsync.py --port COM3 --ls imu
```

After Claude's download branch is integrated, use its download command with the
remote relative path `imu/0000.csv` and a local `.csv` destination (or the whole
`imu` directory if that command supports directories). Consult that branch's
`python tools/sdsync.py --help` for the exact flag. **This checkout's sdsync only
lists/uploads; it has no download command yet.** No speculative flag or transfer
protocol has been added here. Simulator CSV files can be copied directly.

## Cost and verification

Large allocations use named PSRAM budget owners and are retained for reuse:

| Owner | Payload |
|---|---:|
| `imu.ui` | `sizeof(imu_ui)` = 1,336 bytes |
| `imu.writer` | `sizeof(writer)` = 4,512 bytes, including 4 KiB batch |
| `imu.stream` | `512 * sizeof(hal_imu_point)` = 20,480 bytes |
| `imu.sample_stack` | 4,096 bytes, firmware only |
| `imu.writer_stack` | 6,144 bytes, firmware only |

Sim payload total is **26,328 bytes**; with firmware task stacks,
**36,568 bytes** plus allocator headers. Struct sizes were also checked using the
Xtensa compiler and installed 1.43 SDK configuration. Queue capacity
is 5.12 seconds at 100 Hz, 10.24 at 50 Hz, 20.48 at 25 Hz. A full ring drops new
points and retains older queued data. No allocation falls back to internal DRAM.
Shared status/counter variables use about **300 bytes of static DRAM**; two
**340-byte task control blocks**, mutex/timer and libc/FAT bookkeeping add internal
overhead, which must be measured on the board. Stacks use `nos_mem_alloc` and the
same static task primitive as IDF's caps helper, enforcing the budget and 1 MiB
reserve before creation. The scheduler requires the small TCBs in internal RAM.

The 559-frame tour: **0 late frames**, model p95 **8.4 ms**, maximum **24.4 ms**;
live p95 **8.4 ms**, recording p95 **3.3 ms**. At rest after history settles,
60 consecutive frames send **zero pixels**. Draw performs no sensor/SD reads or
app-level libm; trace geometry and hashes are prepared in tick. Sampling adds up
to 100 twelve-byte I2C reads/second; CSV costs one format and magnitude calculation
per point, about 7–10 KB/s at 100 Hz. Worker CPU, shared-bus latency and SD stalls
are **not priced by the raster model**; there is no board CPU utilization claim.
The separate 355-frame iris launch / background / wake run also has **0 late
frames**, p95 **9.8 ms** and maximum **24.4 ms**.

```powershell
.\sim\nos_build.ps1 -Strict
python tools/imu/check.py
$env:NOS_SD_ROOT = 'build/imu/sd' # create an empty test card root first
.\build\nos_sim.exe --damagecheck tools/imu/tour.txt --out build/imu/damage
```

`tools/imu/*.txt` cover UI/idle, exact cadence with no rendered frames, overflow,
sensor errors, card removal/recovery, allocation failure/retry, directory gaps,
exclusive creation, delete, and interrupted-file tails. `check.py` uses fresh
isolated card directories, validates timestamps/magnitudes, and produces
`build/imu/contact_sheet.png` plus full screenshots and logs. Damage check passed:
**559 frames identical**. The sim disables Windows crash dialogs with
`SetErrorMode` and the CRT `_set_abort_behavior` export (resolved dynamically for
older MinGW import libraries). Failed assertions exit with a nonzero status.

ESP-IDF 5.5 direct-compiler syntax checks passed for all changed firmware/shared
C sources. Full CMake build was attempted but the installed target compiler
launcher panicked with `Failed to get path name. Error code: 5` before compiling
the project. **No linked firmware build, flashing or serial test was performed.**

## Claude's 1.43-B checks

1. Build/link with the working board toolchain; check internal heap and `mem`
   before/after first record. Measure both task stack high-water marks during
   formatting, directory scan, slow SD and errors; verify adequate headroom.
2. Record 60 seconds at each rate. Check row counts near 1,500 / 3,000 / 6,000,
   monotonic timestamp intervals, UTC versus synced time, real orientation/axes,
   and DROP/MISS/ERR. Rates are scheduled targets, not hardware-timestamped FIFO.
3. Repeat 100 Hz through auto-sleep, AOD, app exit and wake; confirm uninterrupted
   rows, working touch/raise/knock/steps, and a working IMU recording chip.
4. Observe NOSPERF with live traces and recording, Wi-Fi/BLE, and the existing
   minute SD logger. Check 0 late frames on hardware; test a slow/nearly full SD,
   missing card and physical removal. SD errors must show without blocking UI.
5. Stop/save, inspect/download CSV with the new sdsync download command, browse
   older/newer files, confirm deletion and restart numbering without overwrite.
   Check reboot/deep-off refusal while recording, and normal display sleep.

Remaining risks: I2C contention and scheduling jitter, unmeasured task stack/SDK
heap use, SD stalls longer than queue capacity, power-loss corruption and the
filesystem's file-size limits. `fflush` is not a guarantee against card/controller
power-loss caching. This is a motion logger, not a lossless sensor FIFO recorder.
