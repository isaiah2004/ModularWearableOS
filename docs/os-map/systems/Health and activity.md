---
title: "Health and activity"
tags: [nos, os-map]
updated: 2026-10-23
---

# Health and activity

## Summary

All paths are repo-relative. Everything below was read from source, not run on the watch.

- **Steps** come from a software pedometer (`core/pedometer.c` over `core/walk.c`), fed from the QMI8658 IMU by the `board-sense` task (`firmware/main/board.c:967-1026`). It runs continuously, **including with the screen off or in AOD**: light sleep is disabled (`firmware/main/nos_power.c:20-61`) and the task never stops except for deep sleep or shutdown (`board.c:967-976`). The chip's hardware step engine is not used.
- **km** = `steps * 0.75 / 1000` (`nos/svc.c:45,444`). **kcal** = `steps * 0.04` (`nos/svc.c:46,445`). Both are fixed constants. Weight, height, age and sex are not used by either.
- **There is no BMR / resting / passive calorie accumulation anywhere.** The only energy figures are step-based `kcal`, a MET estimate inside a Sports session (`nos/workout.c:276`, needs `nos_prefs.sports_mass_kg`), and manually entered food and water. `docs/BACKGROUND_JOBS.md:22` says so explicitly ("not a separate calorimeter or resting-energy model").
- **User profile**: the only body field is `nos_prefs.sports_mass_kg` (uint8, 0 = unset, 20-250 kg). It is set only under Sports > setup > BODY MASS (`nos/apps/fit_workout.c:633,669,697-723`) and is used only by the Sports MET estimate. There is no height, age or sex field in `nos/prefs.h`.
- **Midnight rollover** happens twice, independently: once inside the pedometer, with `board.c` calendar gating, and once in `svc.c` `act_rollover()` for hourly bins, the week array and active minutes. They are not synchronised (see Gaps).
- **Hourly and weekly history** lives in `act_saved` (`nos/svc.c:426-436`), persisted to NVS blob `"nosact"` at most every 10 min, not flushed on shutdown. Today's running total is separately persisted by the board as NVS blob `"pedometer"` every 60 s, only while the display is off or in AOD.
- **Heart rate** is an optional MAX30102 strap module (I2C 0x57, `firmware/main/ext/ext_hr.c`). There is no HR hardware on a bare 1.43-B. It measures on demand (Heart app, Breathe, Sports), plus an autonomous **10-minute background measurement of at most 40 s**, which fills a 144-entry **RAM-only** history. Nothing about HR survives a reboot.
- **Energy screen** (`nos/apps/fit_energy.c`) is **not an energy-balance screen**. It shows the cumulative step-based `kcal` from the SD journal, plus manually logged food (kcal) and water (mL). The footer says "ACTIVITY CALORIES ONLY".
- `nos/motion.c` named in the brief is **animation math** (springs, scroll, bezier), not a health file. It has no step or motion-sensing logic. See [[Engine and scheduling]].

## Components

### Pedometer and walk detector (`svc-pedometer`)
- **Purpose**: count "walking-like cadence" steps and active time per local day. `walk_update()` is a per-sample peak detector. `pedometer_update()` adds the day-rollover and the persisted `pedometer_saved {version, day, steps, reserved, active_ms}` (24 bytes).
- **Algorithm** (`core/walk.c`):
  - Magnitude baseline EMA with a 0.7 s time constant, high-passed, then a 0.05 s low-pass.
  - A peak counts when filtered > `threshold_g` (default 0.10 g, tunable 0.04-0.20), falling below 0.03 g, 80-650 ms wide, peak <= 1.0 g, valley <= -0.04 g, peak-to-valley >= 0.18 g.
  - Peak intervals must be 300-1500 ms with CV <= 0.25 over the last up to 7 intervals.
  - Entry needs **8 coherent peaks** (tunable 4-12). The 8 steps are credited retroactively, then +1 per peak.
  - A bout ends after 2.5 s without a peak.
  - Samples with |a|^2 < 0.04 or > 9 g^2, or |gyro| > 250 dps, invalidate the bout. A gap > 100 ms restarts.
- **Where it runs**: inside task `board-sense` (`board.c:989`). The pedometer is fed `imu_take()`'s output, which is the 500 Hz IMU read smoothed by a ~3 Hz one-pole (`board.c:298-313`).
- **Cadence**: one update per ~20 ms loop (50 Hz). The 20 ms cycle is the sum of 8 ms + 8 ms + 4 ms delays and two interleaved `knock_poll()` reads (`board.c:1023-1025`). Screen state is irrelevant.
- **Reads**: `hal_imu` sample, `day` from `calendar_day()`, `tuning_get()` (`walk_g`, `walk_peaks`, every loop, `board.c:990-991`).
- **Writes**: `s_pedometer`, `s_activity` (`hal_activity {steps, active_seconds, walking, valid}`), `s_pedometer_snapshot`, `SP_WALK` journal records on state change (`board.c:994-998`).
- **Files**: `core/walk.c:1-106`, `core/pedometer.c:1-28`, `firmware/main/board.c:960-1026`.
- **Rollover inside the pedometer** (`pedometer.c:11-15`): when `day` changes, `today` is zeroed and `walk_init()` clears candidate peaks. `calendar_day()` (`board.c:134-179`) holds the day undefined (valid = 0) until the timezone and a sane clock (epoch in 2020-2099) are established and no prefs load is pending. It re-evaluates the local date at most once per second.
- **Validity**: `hal_activity_get()` (`board.c:495-506`) returns `valid=0` if prefs are loading, the timezone is not ready, the calendar is unresolved, or the last IMU sample is older than 250 ms.

### Board sensor task (`task-board-sense`)
- **Purpose**: 50 Hz-class IMU acquisition and fan-out. It feeds the pedometer, knock detector, workout motion, wrist detector, sports journal and `hal_imu_sample()` (the filtered sample used by wake and diagnostics).
- **Where it runs**: FreeRTOS task `board-sense`, priority 2, 4096-byte stack allocated by `nos_mem_alloc`, no core affinity (`board.c:1028-1042`). It also samples power every 1 s (`hal_esp32_sample_power`, `board.c:1018`).
- **Runs with screen off**: yes. It parks only in the shutdown handshake (`board.c:969-978`).
- **IMU config** (`board.c:425-432`): QMI8658 at I2C 0x6B, CTRL2 0x24 and CTRL3 0x54 (500 Hz ODR both sensors), 12-byte burst from reg 0x35.

### Board filesystem task (`task-board-fs`)
- **Purpose**: durably save the pedometer day record and the RTC.
- **Where it runs**: task `board-fs`, priority 1, 4096-byte internal stack, 100 ms loop (`board.c:919-965`).
- **Pedometer save**: every 60 s (retry 5 s on failure) **but only if** `s_background_persistence` is set (`board.c:954-955`). That flag is 1 only in the display-OFF/AOD/SENSOR branches (`nos_main.c:2098,2110`) and forced 0 whenever the display is ON (`nos_main.c:2141`). It is also flushed in the shutdown handshake (`board.c:931-945`).
- **Writes**: NVS namespace `wos`, key `"pedometer"` via `hal_store_save_checked` (`board.c:915`, `hal_esp32.c:228-238`). That path writes NVS only, not the SD checkpoint reserved for `"nosprefs"` and `"wifi_cred"`.

### Activity service (`svc-activity`)
- **Purpose**: turn the pedometer total into UI-ready day data: steps, km, kcal, active minutes, 24 hourly bins, 7-day week, "stand hours".
- **Where it runs**: UI task, `svc_tick()` -> `act_tick()` once per 1000 ms (`svc.c:1315-1318`). With the display ON `svc_tick` runs per frame via `nos_bg_tick` (`nos/nos.c:2341`). With the display OFF it runs at 1 Hz from `nos_main.c:2094`. In AOD it runs inside the 1 Hz `nos_frame_run`.
- **Reads**: `hal_activity_get()`, `hal_get_datetime()`, `hal_epoch_seconds()`, `nos_prefs.step_goal_100` (default 80 = 8000, `prefs.c:155`).
- **Writes**: `s_as` (`act_saved`), `s_act` (`svc_activity_t`), `s_sys.steps_today`.
- **Details**:
  - `act_publish()` (`svc.c:440-456`) computes km, kcal, hourly, week[0..5] from history, week[6] = today, `stand_hours` = hours with > 250 steps.
  - Hourly bin: any step delta since the last tick is added to `hourly[current hour]`, clamped to 65535 (`svc.c:542-548`).
  - Active minute: a wall-clock minute counts if it contains **more than 60 steps** (`svc.c:551-556`). This is independent of the pedometer's own `active_ms`.
  - `act_rollover()` (`svc.c:510-527`) shifts the week array by the day gap (1-7). It stores `max_steps` as yesterday, clears hourly and active minutes, and saves immediately. A gap < 0 or > 7 starts a clean week. `valid_days` bits distinguish observed zero from unknown.
  - If `a.valid` is 0, `act_publish(s_as.max_steps)` shows the persisted maximum (`svc.c:533`).
- **Persistence**: NVS `"nosact"` (`act_saved`, version 2), loaded in `act_init()` (`svc.c:562-578`). Saved when dirty and > 10 min since the last save (retry 5 s after a failure), plus on rollover. No flush on shutdown.
- **Simulator**: `#ifndef ESP_PLATFORM` uses a synthetic day, `SIM_DAY[24]`, plus fixed week `{7120, 9340, ...}` (`svc.c:458-479`). The real rollover and save code is device-only.

### Heart aggregation service (`svc-heart`)
- **Purpose**: UI-facing HR state: last reading, resting, today min/max, 24 h bins, last SpO2.
- **Where it runs**: UI task. `heart_tick()` every 60 s (`svc.c:1322`) rebuilds the 144 x 10-min bins from `hal_hr_history_samples()` using producer epochs. `svc_heart_record_at()` is called by apps when they have a locked reading.
- **Semantics**:
  - `resting` = the lowest valid (35-230) sample in the rolling 24 h (`svc.c:597,612`). It is not a true resting-HR algorithm.
  - `bpm_last` is the newest valid sample, from history or an app, **with no expiry**.
  - Day min/max come from samples after local midnight.
- **Persistence**: none. `s_heart` and `s_hr_rec_day_*` are RAM only.

### HR driver (`drv-ext-hr`, task `ext`)
- **Purpose**: MAX30102 acquisition, DSP, beat detection, SpO2, on-wrist detection and the 144-entry background history.
- **Where it runs**: `hr_service()` is called from task `ext` (priority 0, no affinity, 4 KB PSRAM stack, loop wait <= 50 ms, `ext_sys.c:144-166`). The FIFO is drained every 120 ms in one burst while running (`ext_hr.c:256-307`).
- **Acquisition** (`ext_hr.c:100-115`):
  - SpO2 mode, 400 sps with 4x averaging = 100 Hz, 18-bit.
  - LEDs start at 0x1F (~6.2 mA) and auto-range once a second (`auto_led`, lines 205-215).
  - Shutdown (SHDN) when not wanted.
  - **Gated by** `hal_hr_request(owner,on)` with owners WORKOUT, HEART, BREATHE and LEGACY. A lease expires if no `hal_hr_get()` arrives for `NOS_EXT_IDLE_MS` = 8 s (`ext_hr.c:238`).
- **DSP**:
  - DC EMA 0.02, 4 Hz biquad low-pass, PPG = -AC(IR).
  - Beats are local maxima above 0.4 x the envelope, refractory 330 ms, IBI 40-180 bpm. Lock needs >= 4 intervals with MAD/median < 15 %.
  - bpm = 6000 / median IBI. Quality = regularity x perfusion proxy.
  - SpO2 uses the Maxim quadratic on 4 s windows, reported only when quality >= 0.5. The code says not medical.
  - On-wrist = IR DC > 25000 for 0.5 s. Results older than 2 s read back as zero (`hal_hr_get`, lines 351-354).
- **Background measurement**:
  - First one 2 min after boot (`hr_init`, line 322), then every 10 min (`BG_EVERY_MS`).
  - It runs up to 40 s. It ends after 8 s of lock at quality >= 0.6, or after 5 s if not on wrist.
  - It pushes bpm, or 0 for no reading, into `hist[144]` (`hist_push`, lines 217-228).
  - **It runs regardless of screen state, and whether or not the watch is worn.** If a user stream is already live it just copies `cur.bpm`.
- **Journal side effects**: `sports_emit(SP_PPG, red/IR)` per sample (100 Hz) and `SP_HEART` per FIFO batch (lines 284, 296).
- **Presence**: `hr_init()` returns without setting `EXT_HR` if no 0x15 part id is read at 0x57 (lines 310-316). Apps then show ABSENT / "NO HR SENSOR".

### Heart rate app and widget (`app-heart-rate`)
- **Purpose**: live PPG trace, lock to a BPM, SpO2, plus a "LAST 24 H" page.
- **Where it runs**: `nos/apps/fit_heart.c` (screen `scr_heart`, widget `wg_heart`). It requests the HR stream while open (`HAL_HR_OWNER_HEART`, lines 537, 553, 612). It keeps the display awake while measuring (line 671). On leave it records the locked reading with `svc_heart_record_at` (line 554).
- **Lock rule**: on wrist, quality >= 0.6, bpm 35-230, 0.5 s of good signal plus 2 s of contact. A heartbeat haptic fires on lock.
- **Reads**: `hal_hr_get`, `svc_heart()` (history for the page).
- **Widget**: refreshes every 60 s; shows "JUST NOW / n MIN AGO / n H AGO" from `bpm_last_epoch` (lines 952-970).
- **Writes**: `svc_heart_record_at()` on each fresh locked sample, which updates last, min and max only. It does **not** add to the 24 h history.

### Breathe app (`app-breathe`)
- **Purpose**: paced breathing (1/3/5 min, 5 cycles/min) with optional HR.
- **HR**: `HAL_HR_OWNER_BREATHE`, quality >= 0.65, trapezoidal mean of acquisition-time samples. At session end, if >= 10 s and >= half the session was valid, it calls `svc_heart_record_at(avg, 0, epoch)` (`fit_breathe.c:339-355`).
- **Persistence**: none. There is no session count or mindful-minutes record.

### Activity app and widget (`app-activity`)
- **Purpose**: 3 pages: step ring toward the goal, TODAY (km/mi, KCAL, ACTIVE min, 24 hourly dot columns), WEEK (7 bars plus daily average over valid days).
- **Where it runs**: `nos/apps/fit_activity.c`, UI task. Data is a copy of `svc_activity()`.
- **Notes**: the label is plain "KCAL" (line 455). Units follow `nos_prefs.units_metric` (km x 0.621371 for miles).
- **Widget**: `wg_activity`, tap opens the app.

### Energy app and siblings (`app-energy`, `app-battery-history`, `app-sensor-logs`, `app-quick-report`, `app-wrist-tuning`)
- **Where**: all in `nos/apps/fit_energy.c`; one state struct per screen kind (lines 15-50). Registry entries are in `nos/apps/registry.c:9-17`.
- **Energy (kind 0)**:
  - Header = `"%.0f BURN / %.0f EAT / %.0f mL"` for the selected day (lines 141-146).
  - **BURN**: for today at full range it is the live `svc_activity()->kcal`. For other days or zoomed ranges it is the maximum bin value from the SD journal.
  - Chart metric cycles BURNED, FOOD, WATER per 24 bins.
  - **BURNED bins are cumulative step-kcal, not per-interval**: `bin_record` takes the last `SP_ACTIVITY` `v[1]` in each bin (`sports.c:409-411`). The label is "BURNED: DAILY ESTIMATE".
  - Tapping a bar shows `"%.0f kcal"`.
  - FOOD/WATER come from `sports_intake()` (+/-50/250/1, up to 10000), with undo.
  - The history query is async through `sports_query_request/poll`. Refresh is every 10 s. With no SD it shows "HISTORY UNAVAILABLE".
  - There is **no net balance, no BMR, no goal**.
- **Battery history (kind 1)** shows `SP_BATTERY` bins. See [[Power and battery]].
- **Sensor logs, Quick report, Wrist tuning (kinds 2-4)**: event/raw record browser, labelled-trial reports, and `tuning_adjust()` for walk threshold, entry peaks and knock thresholds. See [[Sports and workouts]] and [[Settings and developer]].

### Wrist side detection (`svc-wrist-detect`)
- **Purpose**: experimental "which wrist" guided template match (not a passive detector).
- **Flow**: `wrist_detect_begin(mode, refs)` starts a 4 s capture (hold still 0.6 s, rotate palm up, hold still). `wrist_detect_feed()` is called from `imu_take()` for every raw IMU read in `board-sense` (`board.c:303`) under a spinlock. `wrist_detect_get()` is polled by the UI. Validity needs arc 60-220 deg, >= 85 % single-axis, quiet end >= 400 ms, and a match within 10 half-degrees and 25 % of a learned reference.
- **Consumers**: only the Sports setup screens (`fit_workout.c:604-800`). They write `nos_prefs.wrist_left` (via `prefs_set_wrist`) and `nos_prefs.wrist_refs[4]`. The result is a suggestion that must be confirmed. `wrist_left` also feeds raise-to-wake orientation (`nos_main.c:944`).
- **Persistence**: prefs (SD authoritative, NVS fallback).

### IMU sample/write workers (`task-imu`, `timer-imu-rate`)
- **Purpose**: `firmware/main/imu_hal.c` is the **IMU recorder/stream backend** (IMU app): tasks `imu-sample` (priority 3) and `imu-write` (priority 1), plus an esp_timer `imu-rate`. It is **not** involved in step counting.
- **Details**: `imu_backend_start(hz)` starts the periodic timer, which notifies `imu-sample`, which calls `board_imu_raw()`. It skips late slots. `imu-write` wakes every 20 ms while recording, otherwise every 1 s. Both tasks are created lazily by `imu_backend_wake()` (lines 68-97).
- **Contention**: it shares the QMI8658 with `board-sense` through the per-device lock in `board_imu_raw`.

### Sports journal producers for health (`svc-sports-logger`, `task-sports-sd`)
- **Purpose**: the only durable per-minute activity record.
- **Flow**:
  - `sports_tick()` (1 Hz in `svc_tick`) emits `SP_ACTIVITY {steps, kcal, active_min}` and `SP_BATTERY` once per 60 s (`sports.c:386-391`).
  - Task `sports_sd` (priority 1, PSRAM stack) drains the 1024-record queue every 1 s to `/sdcard/watch/sports/YYYYMMDD-events.spt`.
  - Raw types (IMU, HEART, PPG, WALK) go to hourly `YYYYMMDD-HH-sensors.spt`.
  - Records are 64 bytes (`sports_record`).
- **Health producers**: `sports_imu(m)` per raw IMU read (~150/s, `board.c:300`), `SP_WALK`, `SP_HEART`, `SP_PPG`, `SP_ACTIVITY`.

### Background jobs view (`svc-background-jobs`, `app-jobs`)
- `nos/background.c:25-26` defines "Step tracking" and "Calorie estimate". It samples `svc_activity()` every 1 s and states "0.04 kcal per step". "Heart sampling" is on-demand with a 4 s stale threshold.

### Consumers elsewhere
- `face-segment` rings: steps/goal, kcal/**400**, active min/**30**, goals hard-coded (`face_segment.c:266`). It falls back to `steps*0.04` if `kcal` is 0 (line 263).
- `face-split` shows `bpm_last` (no age check) and tints red at >= 120 or < 40 (`face_split.c:76,109`).
- `face-inverted` shows steps text. `sys-home` builds `face_ctx.steps/bpm/step_goal` from `svc_activity()` and `svc_heart()->bpm_last` (`sys_home.c:469-473`).
- **Gadgetbridge** (`svc-gadgetbridge`): `{"t":"act"}` starts a realtime stream every `int` s (1-600), sending `stp` = step delta since the last send and `hrm` = `s_heart.bpm_last` (`svc.c:1141-1146,1180-1188,1243-1246`). `actfetch` history is not implemented (`svc.c:1190`).

## How it runs

| Stage | Context | Rate | Screen off? |
|---|---|---|---|
| IMU read + filter + `walk_update` | `board-sense`, prio 2, any core | ~50 Hz walk, ~150 Hz raw reads | Yes |
| Pedometer snapshot -> `s_activity` | same | each loop | Yes |
| `calendar_day()` local-date check | same | <= 1/s | Yes |
| `save_pedometer()` to NVS `pedometer` | `board-fs`, prio 1 | every 60 s if dirty and persistence allowed | **Only in OFF/AOD/SENSOR**, never while the display is ON |
| `act_tick()` (hourly, active min, rollover, publish) | UI task via `svc_tick` | 1 Hz | Yes: 1 Hz via `nos_bg_tick` (OFF), 1 Hz frame (AOD/SENSOR) |
| `act_save()` NVS `nosact` | UI task (direct NVS commit in-frame) | dirty and every 10 min, or on rollover | Yes |
| `heart_tick()` | UI task | 60 s | Yes |
| `hr_service()` | `ext`, prio 0 | 120 ms while the sensor runs; 1 s idle | Yes |
| Background HR | `ext` | first at 2 min after boot, then every 10 min, <= 40 s | Yes, and off-wrist too |
| `sports_tick` -> `SP_ACTIVITY` | UI task | 60 s | Yes |
| `task-sports-sd` drain | prio 1 | 1 s | Yes |

- **Screen-off service dispatch**: in PWR_OFF, `nos_main.c:2092-2105` (service call at 2094) calls `nos_bg_tick(1.0f)` (i.e. `svc_tick` + `background_tick`) every 1000 ms when the host owns UI mutation (`mutate`). `mutate` is 0 during OTA activity or shutdown phase 3, and then services do not tick.
- **Power**: 240/240 MHz or DFS 240-80 MHz, `light_sleep_enable=false` (`nos_power.c:20-61`). Deep sleep (long press) stops every sensor and counter after the final `save_pedometer`.
- **HR policy**: owners bitmask, lease renewed by `hal_hr_get`, 8 s idle timeout. Workout renews it from `workout_tick` and `sample_hr` (`workout.c:316-318`).

## Data in / data out

| Data in | From | Used for |
|---|---|---|
| QMI8658 accel/gyro (500 Hz ODR, +/-8 g, +/-512 dps) | `hw-qmi8658-imu` -> `board_imu_raw` | walk, wrist detect, knock, sports journal |
| Wall clock + timezone | `hw-pcf85063-rtc`, phone sync, `hal_set_tz_min` | day key for rollover, hourly bins, HR midnight |
| `nos_prefs.step_goal_100`, `units_metric`, `sports_mass_kg`, `wrist_refs`, `wrist_left` | prefs | goal ring, display units, Sports MET, wrist match |
| `sportstune` walk_g / walk_peaks | `tuning.c` (NVS) | detector threshold and entry peaks, every loop |
| MAX30102 FIFO (red/IR 18-bit, 100 Hz) | `hw-strap-hr` | bpm, SpO2, quality, PPG wave |
| Food/water entries | Energy screen | NVS ledger, journal |

| Data out | To | How |
|---|---|---|
| `hal_activity` | svc-activity, workout, background, serial `steps` | getter under `s_state_lock` |
| `svc_activity_t` | Activity app/widget, faces, home ctx, sports, background, Energy | `svc_activity()` pointer |
| `svc_heart_t` | Heart app/widget, face-split, home ctx, Gadgetbridge | `svc_heart()` pointer |
| `SP_ACTIVITY`, `SP_WALK`, `SP_HEART`, `SP_PPG`, `SP_IMU` | SD journal | `sports_emit` queue |
| Realtime act | phone | `{"t":"act","stp":d,"hrm":bpm_last,"rt":1}` |

## Persistence

| Item | Where | Format | Cadence | Survives |
|---|---|---|---|---|
| Today's steps + active_ms + day | NVS ns `wos`, key `pedometer` | `pedometer_saved` 24 B | 60 s, display off/AOD only; also shutdown | reboot, flash if NVS preserved; **not SD-mirrored** |
| Hourly[24], week[6], active_min, day, last/max steps | NVS `wos` / `nosact` | `act_saved` v2 | dirty and 10 min; on rollover | reboot (last up to 10 min stale); not flushed at shutdown |
| HR 144-entry history | PSRAM (`hr_state.hist`) | `hal_hr_sample` | n/a | **lost on every reboot** |
| `bpm_last`, resting, min/max, SpO2 | RAM (`s_heart`) | struct | n/a | **lost on reboot** |
| Step goal, units, mass, wrist | prefs, SD authoritative (`/watch/settings.N.bin`), NVS fallback | `nos_prefs_t` | debounced | yes |
| Walk tuning | NVS `sportstune` | `sports_tuning` | on adjust | yes |
| Per-minute `SP_ACTIVITY` (the Energy chart source) | `/sdcard/watch/sports/YYYYMMDD-events.spt` | 64 B records | 60 s emit, 1 s drain | yes, if SD present |
| Raw IMU/PPG/heart/walk | `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt` | 64 B records | continuous | yes; no pruning in code |
| Food/water ledger | NVS `sportsfood` (128 x 64 B + header = 8200 B), mirrored to SD journal | struct | on entry | yes |

NVS is a 24 KB partition shared with prefs fallback, wifi, voice history and others (`firmware/partitions.csv:5`). See [[Storage and persistence]].

## Interactions

- [[Engine and scheduling]]: `svc_tick` hook (`nos_bg_tick`), 1 Hz screen-off service dispatch, `nos/motion.c` animation (misfiled).
- [[Services hub]]: `svc-activity`, `svc-heart`, Gadgetbridge act stream, `svc_init` order.
- [[Sports and workouts]]: `hal_activity` step deltas, `HAL_HR_OWNER_WORKOUT`, `sports_mass_kg`, MET kcal, `wrist_detect`, SP_* journal.
- [[Power and battery]]: HR LED duty, 80 MHz idle, background persistence gating.
- [[Storage and persistence]]: NVS `pedometer` / `nosact`, SD journal.
- [[Hardware and drivers]]: QMI8658 config, MAX30102 strap, shared I2C bus.
- [[Shell and faces]]: face-segment rings, face-split HR/steps, `sys-home` ctx, widgets.
- [[Utility apps]]: Jobs ("Step tracking", "Calorie estimate", "Heart sampling").
- [[Settings and developer]]: step goal, units, wrist tuning, Diagnostics HR probe (`set_diag.c:388`).
- [[Connectivity and voice]]: Gadgetbridge realtime act.
- [[Calorie trace]]: kcal path summary below.
- [[Gaps]], [[OS overview]].

### Calorie path at a glance
`IMU -> walk peaks -> steps -> svc act_publish: km = steps*0.75/1000, kcal = steps*0.04 -> { Activity KCAL, face-segment ring (goal 400), Energy BURN, per-minute SP_ACTIVITY journal -> Energy cumulative bars, Jobs "Calorie estimate", Gadgetbridge (steps only) }`. A separate Sports path computes `MET*3.5*kg/200*minutes` only when mass is set. No BMR, no food-to-balance math.

## Gaps

| # | Severity | Gap | Evidence | User impact |
|---|---|---|---|---|
| 1 | high | No BMR/resting/passive energy anywhere. "KCAL" is only `steps*0.04`, unlabelled as active-only on Activity and faces. | `nos/svc.c:46,445`; `fit_activity.c:455`; `face_segment.c:263`; `docs/BACKGROUND_JOBS.md:22` | Daily "burn" is a fraction of real expenditure; 6000 steps shows ~240 kcal. |
| 2 | medium | No user profile: no height/age/sex, kcal and stride ignore weight. Mass lives only in Sports setup (uint8, 0 = unset) and feeds only the Sports MET. | `nos/prefs.h:37`; `fit_workout.c:633,669`; `svc.c:45` | km/kcal are the same for everyone. Weight set elsewhere never reaches Activity/Energy. |
| 3 | medium | Midnight rollover race: pedometer and `svc` detect the new day independently (<= 1 s lag each). If `svc` goes first, `max_steps` is seeded with yesterday's total and stays polluted. | `board.c:134-179`; `pedometer.c:11-15`; `svc.c:510-541` | The next midnight can store the wrong total in `week[5]` (max of old vs new). The week chart and average are wrong about every other midnight. |
| 4 | medium | `nosact` is not flushed on shutdown/sleep, only every 10 min. After a reboot the unsaved delta lands in the boot hour. A reboot spanning midnight loses up to 10 min of yesterday's final total. | `svc.c:558`; no `svc` flush in `nos_main.c` shutdown | Hourly chart and week history are misattributed or short. |
| 5 | medium | All HR history/state is RAM only: 144-entry ring in PSRAM, `s_heart` in RAM. | `ext_hr.c:61`; `svc.c:72` | After any reboot the 24 h HR page, resting and min/max are empty, and the first background sample is 2 min after boot. |
| 6 | medium | `face-split` and Gadgetbridge `hrm` use `bpm_last` with no age limit. The face tints red at >= 120 or < 40. | `face_split.c:76,109`; `svc.c:1145`; `sys_home.c:473` | A stale reading from hours ago is shown (and flagged) as the current heart rate. |
| 7 | medium | Raw sensor journaling is unconditional. `sports_imu()` runs for every IMU read (~150/s) plus PPG at 100 Hz when HR is on, 64 B each, with no decimation or pruning. Inferred from code, not measured. | `board.c:300`; `ext_hr.c:284`; `sports.c:154,254-259`; `grep unlink` finds none | Potentially ~9.6 KB/s of SD writes (~800 MB/day) and SD wear, which would fill the card. Needs on-device check. |
| 8 | medium | HR hardware is an optional strap. A bare 1.43-B has none, so HR/SpO2 apps, widget and faces show ABSENT / "--". | `ext_hr.c:310-316`; `fit_heart.c:266,538 (M_ABSENT)` | No heart data without the module; the OS does not say so beyond "NO HR SENSOR". |
| 9 | medium | "Active minutes" uses a >60 steps/min heuristic (`svc.c:551-556`). The pedometer's own `active_ms/active_seconds` is persisted but only read by serial commands. `move_goal_10` is unused. `face_segment` hard-codes 400 kcal and 30 min goals, so the kcal ring is just the steps ring rescaled. | `svc.c:551-556`; `nos_main.c:1619`; `prefs.h:27`; `face_segment.c:266` | Two active-time definitions; the "move" goal is not configurable. |
| 10 | medium | Energy BURN chart is cumulative, not per-interval. Zoomed ranges show a cumulative value. Needs the SD journal for history, with no net balance or goal. | `sports.c:409-411`; `fit_energy.c:94-97,141-147` | "BURNED" bars rise monotonically and say little about energy use in each hour. |
| 11 | medium | `act_save()` (NVS commit, possibly a flash erase) runs inside the UI task during frames with the display ON. | `svc.c:496,558,527`; `hal_esp32.c:177-186` | An occasional frame hitch every 10 min or at midnight. |
| 12 | low | Pedometer NVS save only runs while the display is off/AOD. A crash or reset during a long screen-on session loses all steps since the last save. | `nos_main.c:2141`; `board.c:954` | Up to one screen-on session of steps lost on a hard reset. |
| 13 | low | Step detector is a first-field-trial design: 8-peak entry drops short walks; > 250 dps or interval < 300 ms ends the bout (fast running); no arm-swing means no steps; no wear detection (steps accrue on the charger/desk/vehicle if vibration is coherent). | `walk.c:5-12,69-79` | Under/over-count; unvalidated. |
| 14 | low | `stand_hours`, `checkpoint_pending/errors/epoch` are computed but never displayed. | `svc.h:97,99-100`; no readers | Dead data; the user cannot see a failing activity save. |
| 15 | low | Breathe's session-average HR is recorded as `bpm_last/min/max` but not in the 24 h history. Neither are Heart-app readings. No mindful-minutes record. | `fit_breathe.c:353`; `svc.c:625-643` | A deliberate slow-breathing average can become the day's "min". |
| 16 | low | `svc_heart_record_at` rejects readings with an untrusted/earlier epoch once a stamped one exists. | `svc.c:628-631` | After a clock change or without a clock, live readings silently never update `bpm_last`. |
| 17 | low | Background HR fires LEDs for >= 5 s every 10 min even off-wrist and with the screen off. | `ext_hr.c:301-304` | Small battery cost, and the history fills with 0s when not worn. |
| 18 | low | Gadgetbridge `actfetch` / history sync is not implemented, so the phone cannot read hourly/week/HR data. | `svc.c:1190` | Health data cannot leave the watch except via SD files. |
| 19 | low | The simulator fakes the day (`SIM_DAY`, synthetic HR history); the real rollover and save paths are device-only. | `svc.c:458-479,482-`; `sim/nos_ext_sim.c:714-753` | Sim behaviour does not validate persistence or rollover. |
| 20 | low | `wrist_detect` is an experimental, guided two-template matcher consumed only by Sports setup. | `wrist_detect.h:3-8`; `fit_workout.c:604-800` | The result is only a suggestion; nothing else can tell which wrist the watch is on. |
| 21 | low | NVS partition is only 24 KB and holds sportsfood (8.2 KB), voice history, prefs fallback, wifi and more. Saves need free space for the new copy. Not measured. | `partitions.csv:5`; `sports.h` record size 64 | A food-ledger or pedometer save could fail with NOT_ENOUGH_SPACE as NVS fills. |
| 22 | low | `act_publish(max_steps)` shows the persisted maximum while `hal_activity` is invalid, including the first boot seconds and before the date is resolved. | `svc.c:533` | A stale or yesterday's step count briefly appears after boot. |
