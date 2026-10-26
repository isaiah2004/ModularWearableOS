---
title: "Sports and workouts"
tags: [nos, os-map]
updated: 2026-10-23
---

# Sports and workouts

## Summary

Workout recording on the 1.43-B is split into four independent pieces. They share the QMI8658 IMU and the SD card but almost nothing else.

1. **Workout session engine** (`nos/workout.c`, UI in `nos/apps/fit_workout.c`). 40 selectable sport profiles. One session at a time. It stores aggregates only (active time, HR average and peak, GPS distance, steps, laps, wrist-movement time and "bursts", a category-MET calorie estimate). The last 12 sessions live in two alternating checksummed banks, `/sdcard/watch/workouts/workouts.{0,1}.bin`.
2. **Sports sensor journal** (`nos/sports.c`). An always-on 64-byte-record event and sensor journal. It is not tied to workouts. Raw 6-axis IMU (about 150 Hz), HR and PPG records land in hourly files `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt`. Per-minute battery and step records go to `YYYYMMDD-events.spt`.
3. **Raw IMU CSV recorder** (`nos/imu_rec.c` + `firmware/main/imu_hal.c`). A manual recorder started from the IMU app at 25, 50 or 100 Hz. It writes `/sdcard/imu/NNNN.csv`. It does not run with a workout unless the user starts it by hand.
4. **Host tools** (`tools/sports_export.py`, `tools/workouts_export.py`, `tools/imu/audit.py`, `tools/sdsync.py`). All are offline.

For a **BADMINTON** session (`WORKOUT_BADMINTON = 8`, profile `{"BADMINTON","TIME / HEART + MOVEMENT",outdoor 0,uses_steps 0,show_speed 0,family RACKET(3),MET 5.5}`, `nos/workout.c:65`):

| Question | Answer from the code |
|---|---|
| What is recorded in the session bank | Active time (`elapsed_ms`, pauses excluded), total observed time, HR average and peak and HR-valid milliseconds, up to 20 manual laps, wrist-movement milliseconds, movement "bursts" and motion-valid milliseconds, a quality-rejection counter table, frozen wrist and hand setup, frozen MET and mass, and `energy_kcal_est`. No steps for badminton (`uses_steps=0`, `workout.c:288`). No distance or GPS (indoor). |
| Swings | **No.** The motion engine counts "movement episodes", explicitly "never shots/repetitions" (`nos/workout_motion.h:5`). There is no swing, shot or rally detector anywhere. |
| IMU sample rate feeding the session | About 150 Hz. `board-sense` reads the QMI8658 three times per 20 ms (t=0, +8, +16 ms) and each read calls `workout_motion_sample()` (`firmware/main/board.c:298-314`, `:987-1030`). The chip runs at 500 Hz ODR, +/-8 g, +/-512 dps (`board.c:425-426`). |
| HR sampling | Optional MAX30102 strap (`hw-strap-hr`): 100 Hz PPG, FIFO drained every 120 ms. The workout polls the cached result at most once per second and applies quality, range, freshness and spike gates (`workout.c:314-354`). Without the strap every sample is `HR_MISSING`. |
| Where it is written | The summary is written to `/sdcard/watch/workouts/workouts.{0,1}.bin` (4304-byte `disk_record`, version 3) by `task-workout-sd`. A checkpoint is written every 30 s while running and on every state change. The raw IMU and HR journal is written independently to `/sdcard/watch/sports/`. |
| Calories | Yes, but only a gross category estimate: `kcal = MET x 3.5 x mass_kg / 200 x active_minutes` (`workout.c:276-278`). It uses the profile MET (5.5 for badminton) and body mass from the Sports setup. It does **not** use HR, movement, age, sex or height. It is zero if mass is unset, and mass defaults to unset. See [[Calorie trace]]. |
| Calories in the daily totals | **No.** The daily "burn" is `steps x 0.04` (`nos/svc.c:46,445`). Nothing adds `energy_kcal_est` to it (checked: `fit_energy.c`, `fit_activity.c` and `svc.c` never reference the workout energy). |
| Per-session graph or timeline | **None on the watch.** The session has no time series. Offline, the 150 Hz journal can be filtered to the session window with `tools/sports_export.py`, but nothing aggregates it into a movement-intensity-over-time series or a plot. Only a manually recorded IMU CSV can be plotted, with `tools/imu/audit.py` (matplotlib SVG/PNG). |
| Summary screen (indoor sport) | Title, save status ("SAVING TO SD" / "SAVED TO SD" / "SAVE FAILED / RETRY"), large active time, "TOTAL DURATION", then one line "AVG HR n BPM / n LAPS" (or "HR -- / n LAPS"), then DETAILS and DONE buttons (`fit_workout.c:391-419`). No calories, movement or bursts on the summary. They appear only in the DETAILS list (`:955-1040`). |

## Components

### app-sports: Sports app (`scr_workout`)
- **Purpose:** profile picker (6 favourites plus a 40-entry catalog), ready screen, live view, pause/resume/lap, finish confirmation, summary, history of 12 sessions, session details, setup (wrist, dominant hand, playing hand, bowling hand, auto-resume, body mass, guided wrist detection), body-mass editor.
- **Where it runs:** UI thread screens. They are `SCR_LIVE_RENDER`, never `SCR_KEEPAWAKE`. The session itself lives in `svc-workout`, so leaving the app never pauses or ends it (`fit_workout.c:1-3`). Opening the app mid-session returns to the live view (`view_for_session`, `:325-331`).
- **Cadence:** every frame while open: `snapshot()` copies `workout_get()` and `workout_storage_get()` (`:243-295`). The history list refreshes every 1 s (`:897-902`).
- **Reads:** `workout_get`, `workout_history_get`, `workout_storage_get`, `nos_prefs.*` (units, wrist, sports_*), `wrist_detect_get`.
- **Writes:** `workout_start/pause/resume/lap_add/finish/retry_save`, `workout_setup_set`, `workout_placement_revision_set`, `workout_prepare/unprepare`, `prefs_save()`, `prefs_set_wrist()`, `diag_action_*`.
- **Files:** `nos/apps/fit_workout.c:1058-1066` (screens), `nos/apps/registry.c:10` (launcher entry "Sports", `DOT_ICON_STOPWATCH`).
- **Badminton specifics:**
  - Tile 3 of the favourites grid (`fit_workout.c:31-34`).
  - The live view for a non-GNSS, no-step sport shows BPM and "BURSTS / EST." plus a caption "WRIST MOVEMENT / NOT REPS" (`:376-386`). The caption is "LEFT WRIST / RIGHT PLAY"-style for racket sports.
  - `uses_playing_hand` covers the RACKET family, cricket and volleyball (`:97-101`).
  - Start is refused until the SD history has loaded (`:536-556`, `workout.c:817`).

### svc-workout: session engine (`workout.c` / `workout.h`)
- **Purpose:** the state machine IDLE -> RUNNING <-> PAUSED -> FINISHED. It accumulates active time, HR, GPS, steps and cadence, laps, energy and motion aggregates, and owns the checksummed SD history.
- **Where it runs:** `workout_tick()` is called from `svc_tick()` (`nos/svc.c:1321`). That is every UI frame while the screen is on (`nos.c:2341`). It is called at 1 Hz when the display is off (`firmware/main/nos_main.c:2094`) and at the 1 Hz AOD/sensor-idle frame (`nos_main.c:2103-2110`, inferred).
- **Cadence:**
  - Time advances every tick (`advance_time`).
  - Sensors are sampled at most once per `SAMPLE_MS=1000` (`:46,805`).
  - A checkpoint is requested every `CHECKPOINT_MS=30000` while running (`:47,806`) and on every state change or lap.
- **Reads:**
  - `hal_activity_get` (pedometer day total; the session step delta is computed with spike and gap guards, `:286-313`).
  - `hal_hr_get` plus `hal_hr_request(HAL_HR_OWNER_WORKOUT)`, renewed every second.
  - `hal_gps_get` plus `hal_gps_enable(1)` for outdoor profiles.
  - Wrist-motion aggregates from `eng-workout-motion`.
  - `hal_epoch_seconds`, `esp_timer`.
- **Writes:**
  - `current` and `history[12]` in RAM.
  - A pending disk image for `task-workout-sd`.
  - `diag_log_event(DIAG_WORKOUT, id, state, sport)` and `DIAG_WORKOUT_RECOVERY`.
- **Calorie rule (`advance_time`, `:268-280`):**
  - `energy_kcal_est = met_tenths*0.1 * 3.5 * mass_kg / 200 * elapsed_min`.
  - The MET is frozen at start from the profile table (`:56-97`). Mass comes from `nos_prefs.sports_mass_kg` via `configure_session()` (`fit_workout.c:102-115`).
  - It is zero when `mass_kg==0` or `met_tenths==0`. It accumulates only in RUNNING, so a pause freezes it.
- **HR gates (`sample_heart`, `:314-354`):**
  - `on_wrist` required.
  - `quality` in [0.65, 1.0].
  - 35..230 bpm.
  - Freshness 3 s (`HR_FRESH_MS`).
  - Sample not older than the segment start.
  - Monotonic sample order.
  - A jump of more than 30 bpm within 10 s needs 3 consistent samples.
  - Average HR = trapezoid integral over accepted gaps of at most 3 s.
- **Profile table:** 40 sports, with family and `met_tenths` (e.g. badminton 55, tennis 68, table tennis 40, squash 73, run 75, jump rope 110).
- **Persistence:** see `store-sd-workouts`.
- **Recovery:** on boot the writer loads the newest valid bank. A RUNNING or PAUSED session is marked `recovered` and resumes if `auto_resume`, otherwise it comes back PAUSED. Downtime is excluded because the clocks are rebased (`apply_storage`, `:658-710`).
- **Files:** `nos/workout.c:1-907`, `nos/workout.h:1-146`.

### eng-workout-motion: wrist-movement episode detector (`workout_motion.c`)
- **Purpose:** turns the IMU stream into `valid_ms`, `movement_ms` and `bursts`.
- **Algorithm:**
  - The input per sample is |a| (g) and |gyro| (dps).
  - A sample is "moving" if `| |a|-1 | > 0.16 g` or `gyro > 55 dps` to enter, with hysteresis `0.10 g` / `35 dps` while active.
  - 300 ms of continuous motion starts an episode (`bursts++`), and 600 ms of quiet ends it.
  - A gap of more than 250 ms between samples counts as a gap and resets continuity.
  - Rejects: non-finite values, |a| > 32 g, or gyro > 4000 dps.
- **Where it runs:** called synchronously from `task-board-sense` inside `imu_take()` (`board.c:298-303`), under the workout `portMUX` spinlock (`workout.c:135-139`). About 150 calls/s. Gated by `motion_enabled`, which is on only while RUNNING.
- **Reads:** raw `hal_imu`. **Writes:** `workout_motion` struct; `motion_sync()` copies it into `current` once per tick.
- **Files:** `nos/workout_motion.c:1-52`, `workout_motion.h`.

### task-workout-sd: workout SD writer (`workout_sd`)
- **Purpose:** the only code that touches the workout files. It loads history, then writes verified checkpoints.
- **Where it runs:** FreeRTOS task `workout_sd`, priority 2, 6144-byte stack in PSRAM, no core affinity (`workout.c:777-778`). Loop: `workout_storage_service(); vTaskDelay(500 ms)` (`:763-765`). The 12.9 KB work buffer (3 x `disk_record`) is allocated from PSRAM (`:772`).
- **Gating:**
  - A write happens only when `storage.dirty` and either `force_write` is set or 5 s (`RETRY_MS`) have passed since the last attempt (`:725-727`).
  - Each write takes the shared SD lock `hal_sports_sd_lock()`, a 50 ms try-take on the board SD mutex (`board.c:93-97`).
- **Write procedure** (`write_checkpoint`, `:593-643`):
  1. Read both banks and classify each.
  2. Refuse if a valid bank is not one of ours (`foreign`), or if a read is unresolved (`uncertain`).
  3. Write the other slot with `fwrite`, `fflush`, `fsync`, `fclose`.
  4. Re-read it and `memcmp` against the image.
  5. Only then ack success.
- **Files:** `nos/workout.c:720-784`.

### svc-sports-logger: Sports journal (`sports.c`)
- **Purpose:** a bounded in-RAM queue and an SD journal of everything the OS observes (power, battery, activity, steps, knock, wake, IMU, climate, light, compass, heart, GPS, PPG, microphone level, touch, lock, routines, user labels, food, water).
- **Producers:** `sports_emit(type,v,n)` is thread safe and is called from `board-sense`, `ext`, `hal_esp32`, `nos_main`, `nos.c` and others. The main raw producer is `sports_imu()` at `board.c:300`: an unconditional SP_IMU record for every raw IMU read.
- **Queue:**
  - `QN=1024` records x 64 B in PSRAM.
  - Raw types (IMU..GPS, PPG..TOUCH) are limited to 896 queued; non-raw to 992; "essential" types (boot, power, report, label, tune, food, water, wake, lock, invalid battery/rail, failed routine) to 1024.
  - Overflow increments `status.dropped`.
  - `RN=96` recent non-raw records are kept in RAM for the Sensor logs app.
- **Observer:** `sports_observe(background observe)` is called outside the lock for every emitted record (`sports.c:145-150`). Jobs data is derived from it.
- **Quiesce:** `sports_quiesce(1)` before shutdown suppresses non-essential records (`:132-144`).
- **Per-minute records (`sports_tick`, `:366-402`):** at 1 Hz it refreshes the clock stamp (epoch, local day, local minute). Every 60 s it emits SP_BATTERY and SP_ACTIVITY (`steps`, `kcal = steps x 0.04`, `active_min`).
- **Files:** `nos/sports.c`, `nos/sports.h`.

### task-sports-sd: journal writer (`sports_sd`)
- **Where it runs:** FreeRTOS task `sports_sd`, priority 1, 4096-byte stack in PSRAM, no affinity (`sports.c:323`). Loop (`:282-293`):
  - `drain()` once per 1000 ms.
  - Then `query_step()` and `diag_log_service()`.
  - Then `ulTaskNotifyTake` with a 1000 ms timeout, or 20 ms while a UI query is pending.
- **`drain()`** (`:243-280`):
  - Takes the SD lock.
  - Writes up to 512 records (`DRAIN_BATCH`).
  - Keeps two open files at once: the daily events file and the current hour's sensors file.
  - Every batch ends with `fflush`+`fsync`+`fclose` on each, so one fsync per file per second.
  - On a failure the queue tail is not advanced and the whole batch is retried, so records may repeat. Replays are de-duplicated later by `(boot,seq)`.
- **Paths:** `ROOT "/sdcard/watch/sports"`. Events: `YYYYMMDD-events.spt`. Raw: `YYYYMMDD-NN-sensors.spt`, where NN = local hour + 1 minus 1 (`path_for`, `:215-219`, `:261`).
- **Also serves:** asynchronous day queries for the Energy and Battery history charts (`sports_query_request/poll`, 128 records per step) and single-record browsing for the Sensor logs app (`sports_archived/sports_raw`).

### svc-imu-recorder: manual raw IMU CSV recorder (`imu_rec.c`, `imu_hal.c`)
- **Purpose:** record 25/50/100 Hz 6-axis IMU to CSV on request. This is the only exportable high-resolution motion timeline, and it is separate from workouts.
- **Start:** the IMU app calls `hal_imu_record_start(hz)` (`nos/apps/out_imu.c:184`). Nothing in `fit_workout.c` or `workout.c` starts it.
- **Tasks and timers:**
  - `timer-imu-rate`: an esp_timer named `imu-rate`, periodic at 1/hz, which only notifies the sampler (`imu_hal.c:24,84-87,104`).
  - `task-imu`: FreeRTOS `imu-sample`, priority 3, 4 KB stack, PSRAM via `nos_mem_alloc`. It calls `board_imu_raw()` directly (so those samples do not pass through `imu_take()`) and pushes to a 512-entry ring (`imu_hal.c:25-44`, `imu_rec.c:132-143`).
  - `task-imu-write`: FreeRTOS `imu-write`, priority 1, 6144 B. It runs `imu_worker_step()` every 20 ms while recording and every 1 s otherwise (`imu_hal.c:45-53,88-91`).
- **Files:**
  - `/sdcard/imu/NNNN.csv`. Header `ms,utc_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,a_g`. 4 MiB segments, then a linked new file. Flushed with fsync at least every 1 s or per 4096-byte block.
  - `NNNN.link`: previous-segment linkage.
  - `/sdcard/imu/intent.{0,1}.bin`: checksummed recording intent for crash-resume.
- **Files in source:** `nos/imu_rec.c:1-471`, `nos/imu_backend.h`, `firmware/main/imu_hal.c`.

### svc-wrist-detect: guided wrist/orientation calibration (`wrist_detect.c`)
- **Purpose:** the experimental left/right wrist detector used from Sports > setup. It needs references learned on both wrists, and requires user confirmation.
- **Where it runs:** fed per sample by `imu_take()` (`board.c:301`); UI-driven begin/get/cancel from the wrist screen (`fit_workout.c:728-801`). Capture window is 4 s.
- **Files:** `nos/wrist_detect.c`, `wrist_detect.h`.

### task-board-sense: IMU producer (`board-sense`)
- **Where it runs:** FreeRTOS `board-sense`, priority 2, 4096-byte stack in internal RAM, no affinity (`board.c:1036`). Loop of `board_imu_raw` (t=0), `vTaskDelay(8)`, `knock_poll`, `vTaskDelay(8)`, `knock_poll`, `vTaskDelay(4)`. That is 3 reads per 20 ms (about 150 Hz). `CONFIG_FREERTOS_HZ=1000`, so the delays are exact (`sdkconfig:1917`).
- **Per read (`imu_take`, `board.c:298-314`):**
  - `sports_imu` (journal).
  - `workout_motion_sample`.
  - `wrist_detect_feed`.
  - A 3 Hz one-pole filter, whose result is what the pedometer and raise-to-wake see.
  - `knock_feed`.
- **Screen-off:** keeps running with the display off. It stops only for deep sleep or restart (`s_stopping` handshake, `board.c:970-980`).

### hw-strap-hr + task-ext: heart-rate source
- MAX30102 at 0x57 on the extension strap. `hr_service()` runs inside the `ext` task (FreeRTOS `ext`, priority 0, 4096 B PSRAM stack, no affinity; `ext_sys.c:186`).
- The FIFO is drained every 120 ms. Each FIFO sample emits an `SP_PPG` record (red, IR). Each drain emits an `SP_HEART` record (`ext_hr.c:284,296`).
- It is enabled only while some owner holds a lease. The workout's lease bit is renewed once per second by `sample_heart`; the idle auto-stop is `NOS_EXT_IDLE_MS = 8000`. A background 10-minute HR history is kept regardless (`ext_hr.c:34`).
- `hal_hr_get()` blanks any reading older than 2 s (`ext_hr.c:351`).

### svc-background-jobs: Jobs (BG_WORKOUT "Sports session")
- Once per second `sample_services()` publishes `{elapsed_s, session_steps, energy_kcal_est}` as job BG_WORKOUT (`nos/background.c:154-160`). The 2-minute rolling window has 120 points (`background.h:14-16`).
- It shows in `app-jobs` and opens `scr_workout` (`nos/apps/sys_background.c:218`). This is the only place the energy figure is charted, and only as a 2-minute live trace.

### sys-home recording chip and shutdown/OTA guards
- `sys_home.c:163-173`: the "SPORT mm:ss" / "PAUSE mm:ss" recording chip on faces. Tapping it opens Sports (`kind 4`).
- `nos_main.c:735-748` (`shutdown_guard`) and `ota.c:56-72` (`ota_poll`) block restart, deep sleep and OTA while a workout is RUNNING, PAUSED, or FINISHED but not yet verified-saved, or while workout storage is dirty or busy. The toast is "SAVE RECORDING FIRST".

### tools: offline exporters (ext-host-pc)
- `tools/workouts_export.py`: reads checkpoint banks (v1, v2, v3) and writes JSON or summary CSV plus lap CSV. Falls back to the older bank if the newest is invalid. It never overwrites.
- `tools/sports_export.py`: streams the `.spt` journals to CSV, one row per record with 8 field/value pairs. It filters by `--day`, `--start/--end` (local minutes) and `--events`. There is no aggregation.
- `tools/imu/audit.py`: replays an IMU CSV through the production `workout_motion.c` and plots acceleration and gyro with the engine's episode shading.
- `tools/sdsync.py --pull DIR`: pulls SD directories over USB or BLE via `sd_xfer`. The workout and sports paths are not "protected" paths there.

## How it runs

**Scheduling**

| Context | What | Cadence | Notes |
|---|---|---|---|
| `board-sense` (prio 2) | IMU read and fan-out (journal, workout motion, wrist detect, knock, pedometer) | 3 reads per 20 ms, about 150 Hz | Runs with the screen off. |
| UI/service thread (`svc_tick`) | `workout_tick` | Per frame when ON; 1 Hz when OFF/AOD/SENSOR | `workout_tick` is O(1). Sensor sampling is at most 1 Hz. |
| UI/service thread | `sports_tick` | Internally 1 Hz; 60 s battery/activity records | |
| `workout_sd` (prio 2) | History load, checkpoint write | 500 ms loop; write on dirty, 5 s retry; checkpoint every 30 s while running | |
| `sports_sd` (prio 1) | Journal drain | 1 s; 512 records per batch; 20 ms when a query is pending | |
| `ext` (prio 0) | HR FIFO and `SP_HEART` / `SP_PPG` emits | 120 ms | Only when a lease is held. |
| `imu-rate` timer, `imu-sample` (prio 3), `imu-write` (prio 1) | Manual CSV recorder | 25/50/100 Hz sampling; 20 ms writer loop | Only while the IMU app has started a recording. |

**Cores and priorities.** Everything above is created with no core affinity (`tskNO_AFFINITY`). The UI (`flush`, `touch`) tasks are pinned to core 1 (`nos_main.c:1887-1889`). Both SD writers take stacks from PSRAM.

**Screen-off behaviour.**
- A workout keeps running. Time, HR, GPS, steps, laps, motion and checkpointing continue with the display off or in AOD. Only deep sleep or power-off stops recording; on the next boot the session recovers from the last checkpoint (up to 30 s lost) with the downtime excluded.
- `board-sense`, the journal and the HR lease continue.
- The sports journal continues to write raw IMU in all power states except when `sports_quiesce(1)` is set during shutdown.

**SD arbitration.** The workout writer, the sports writer, sd_xfer and the IMU writer all contend on the same board SD mutex (`hal_sports_sd_lock`, a 50 ms try-take, `board.c:93-97`). A failed take is counted as `WORKOUT_IO_LOCK` and retried after 5 s.

## Data in / data out

**Data in**

| Source | Data | Consumer | Mechanism |
|---|---|---|---|
| hw-qmi8658-imu via `task-board-sense` | ax..gz, 150 Hz | `eng-workout-motion`, `svc-sports-logger`, `svc-wrist-detect` | direct call in `imu_take` |
| hw-qmi8658-imu via `task-imu` | 25/50/100 Hz | `svc-imu-recorder` ring | `imu_stream_push` |
| hw-strap-hr | bpm, quality, on_wrist, PPG | `svc-workout` | `hal_hr_get` (cached) |
| hw-strap-gps | fix, lat/lon, speed, HDOP, age | `svc-workout` (outdoor profiles only) | `hal_gps_get` |
| pedometer (`hal_activity`) | day step total | `svc-workout` (profiles with `uses_steps`) | `hal_activity_get` |
| prefs | `sports_dominant/racket/batting/bowling/volleyball/mass_kg/auto_resume`, `wrist_left`, `wrist_refs`, `wrist_source/revision`, `units_metric` | `app-sports` -> `workout_setup` | `configure_session()` |
| RTC/clock | epoch seconds | `svc-workout` (started/ended), journal stamps | `hal_epoch_seconds` |

**Data out**

| Sink | Data | Mechanism |
|---|---|---|
| `store-sd-workouts` | 12-session history and the active-session checkpoint | `task-workout-sd` |
| `store-sd-sports-journal` | raw IMU, HR, PPG and per-minute records | `task-sports-sd` |
| `store-sd-imu-csv` | manual raw CSV | `task-imu-write` |
| `svc-background-jobs` | session time, steps, energy | `workout_get` in `background_tick`; journal observer |
| `sys-home` chip | "SPORT mm:ss" | `workout_get` |
| shutdown/OTA guards | busy flag | `workout_get`, `workout_storage_get` |
| diag log | `DIAG_WORKOUT`, `DIAG_WORKOUT_RECOVERY`, quality summaries (at most one per 30 s), I/O errors | `diag_log_event` |
| `svc-heart` | lease on/off for HR | `hal_hr_request(HAL_HR_OWNER_WORKOUT,..)` |
| `hw-strap-gps` | enable | `hal_gps_enable(1)` |

**Not out:** no workout data goes to the phone (no Gadgetbridge message), to the daily activity totals, to the Energy app, or to the Activity rings.

## Persistence

| Store | Location and format | Writer / flush | Survives |
|---|---|---|---|
| Workout history and checkpoint | `/sdcard/watch/workouts/workouts.0.bin` / `.1.bin`. `disk_record` v3: header (magic `0x574f5331`, version, bytes, sequence, count, next_id, `hr_integral`), `session`, `history[12]` of 328-byte snapshots, FNV-1a checksum. Total 4304 B. Older v1 (3104 B) and v2 (3472 B) banks are migrated on read. | `task-workout-sd`. Alternating banks, fsync, read-back compare. Dirty until a verified write; retried every 5 s. A "foreign" or "uncertain" bank blocks overwrite. | Reboot, reflash, deep sleep. Lost: more than 12 sessions back, and up to 30 s of the active session. |
| Sports journal | `/sdcard/watch/sports/YYYYMMDD-events.spt`, `YYYYMMDD-NN-sensors.spt`. Records are 64 B: `<6I2H8fI` (magic `0x53505431`, boot, seq, uptime_ms, epoch, local day, local minute, type, 8 floats, FNV-1a). | `task-sports-sd`, once per second, fsync per batch. | Reboot and reflash. **No pruning code exists.** |
| Food and report ledger | NVS keys `sportsfood` (128 entries) and `sportsmarks` (16 entries) through `hal_store_save_checked` | UI thread on intake or report | Reboot, reflash (NVS is preserved by the checked flash) |
| Raw IMU CSV | `/sdcard/imu/NNNN.csv`, `.link`, `intent.{0,1}.bin` | `task-imu-write` | Reboot (auto-resume into a new linked segment) |
| Sports preferences | in `nos_prefs_t` (`sports_*`, `wrist_*`), SD `/sdcard/watch/settings.{0,1}.bin` with NVS fallback | `prefs_save()` | Reboot, reflash |
| RAM only | `current`, `history[]`, the sports `recent[96]` ring, the Jobs 120-point windows | n/a | Lost on reboot (the workout state is recovered from SD) |

## Interactions

- [[Engine and scheduling]]: `svc_tick`, `nos_bg_tick`, screen-off 1 Hz service loop, `SCR_LIVE_RENDER` screens, iris pushes.
- [[Services hub]]: `svc_init` starts `sports_init` and `workout_init`; `svc_tick` drives `workout_tick`/`sports_tick`; Jobs publisher.
- [[Health and activity]]: pedometer day total feeds session steps; daily kcal (`steps x 0.04`) is a separate, non-merged estimate; HR service and strap.
- [[Calorie trace]]: the badminton diagnosis (no mass, no graph, no BMR). This note is the system-level source for it.
- [[Storage and persistence]]: shared SD mutex and mount lease, bank format and recovery, SD journal growth.
- [[Hardware and drivers]]: QMI8658 config (500 Hz, +/-8 g, +/-512 dps), MAX30102 and GNSS strap modules, the SD card.
- [[Power and battery]]: shutdown/OTA guards, 150 Hz sampling plus per-second SD fsync cost with the screen off, deep sleep stops recording.
- [[Shell and faces]]: the "SPORT" recording chip on faces; Jobs entry points.
- [[Settings and developer]]: wrist, hand and mass prefs; diagnostics events; Developer tools.
- [[Utility apps]]: the IMU app is the only starter of the raw CSV recorder; Sensor logs and Energy read the journal.
- [[Connectivity and voice]]: `sd_xfer`/`ble_xfer` are the only ways to get workout files off the watch.
- [[Gaps]] and [[OS overview]]: see the lists there.

## Gaps

1. **Workout calories never reach the daily totals** (high).
   - `energy_kcal_est` (`workout.c:276`) is consumed only by the DETAILS list (`fit_workout.c:1013-1019`) and the Jobs trace (`background.c:155`).
   - Daily burn is `steps x 0.04` (`svc.c:445`). Energy and Activity never read the workout. Wrist-based badminton is also under-counted by the pedometer.
   - Impact: a 1-hour game adds about 400 kcal to nothing, and the day's "burn" misses it.

2. **Calories are a flat category estimate, hidden, and zero by default** (high).
   - No HR term, no movement scaling, no age, sex, height or BMR.
   - `sports_mass_kg` defaults to 0, so `energy_kcal_est = 0` (`workout.c:276`, `prefs.c:160-176`).
   - No kcal on the live, summary or history screens. With mass unset the DETAILS row is not drawn at all, with no "set body mass" hint (`fit_workout.c:1013`).
   - Waiting time counts at full MET (`fit_workout.c:1018`).

3. **No per-session activity timeline anywhere** (high).
   - `workout_snapshot` has scalars and 20 laps (`workout.h:71-96`). The IMU feed is reduced to counters and discarded sample by sample.
   - Nothing on the watch draws a session chart. The Jobs motion trace covers only the last 2 minutes (`background.h:14-16`).
   - Offline, `sports_export.py` emits one CSV row per 150 Hz record and filters by local minute. There is no session-aware aggregation, no graph, and no workout id in the journal.

4. **Raw IMU journal is unconditional and unbounded** (high).
   - `sports_imu()` is called for every read (`board.c:300`). That is about 150 x 64 B = 9.6 KB/s, about 35 MB per hour and about 830 MB per day. During HR streaming `SP_PPG` adds 100 more records/s (`ext_hr.c:284`) and `SP_HEART` about 8/s.
   - Nothing deletes old `.spt` files (`sports.c`: no `remove`), so the card eventually fills. It is a continuous per-second fsync workload even with the screen off. Each record also hits the Jobs observer under spinlocks.
   - Impact: battery drain, SD wear, and an eventual full card where journal and workout writes fail (`ENOSPC`).

5. **Sensor range clips racket swings and the saturation guard cannot fire** (medium).
   - The chip is configured +/-8 g and +/-512 dps (`board.c:425-426`).
   - `workout_motion_feed` flags saturation only above 32 g or 4000 dps (`workout_motion.c:14`), values the sensor can never report.
   - Wrist rotation in a badminton smash exceeds 512 dps, so peaks are clipped silently. Rejection counters would never show it.

6. **"BURSTS / EST." is a poor live metric for badminton** (medium).
   - The episode detector merges continuous footwork into long episodes. The documented replay of the confirmed games gives 8 episodes in 267 s with 224 s of movement (`docs/SPORTS_RELIABILITY_VALIDATION.md:46-47`).
   - The live screen shows the burst count in large digits (`fit_workout.c:376-383`) but hides movement time and coverage. The summary shows neither. They are only in DETAILS.

7. **No link from a workout to its raw data** (medium).
   - The workout never emits a start/stop/sport marker into the sports journal (no `SP_ROUTINE` from `workout.c`). Its diag events go to the separate diagnostics log. Attribution depends on wall-clock overlap.
   - If the clock is unset, `started_epoch=0` (`workout.c:207-211`) and the journal `epoch` is 0 as well, so nothing can be matched.
   - Also, no option starts the IMU CSV recorder with a sport. A user has to start it manually in the IMU app, in addition to the workout.

8. **A workout cannot start without the SD history loaded** (medium).
   - `workout_start` requires `storage.ready` (`workout.c:817`); the ready screen shows only RETRY (`fit_workout.c:536-556`).
   - With no card, or a corrupt or foreign bank, no sport can be recorded at all, not even in RAM. This is deliberate, to protect card lineage, but it is a hard failure for a card-less watch.

9. **History is capped at 12 sessions, with no per-session file, delete or export on the watch** (low).
   - The 13th finish evicts the oldest session from the only bank (`workout.c:873-876`).
   - `workout_discard()` is not called by any device screen (only `sim/nos_sim.c:966`). A bad or accidental session cannot be removed from history.

10. **Times are shown in UTC** (low).
    - History and details print `YYYY-MM-DD HH:MMZ` and "STARTED / UTC" (`fit_workout.c:827-843,1027`).
    - The watch holds a timezone elsewhere (IST). The displayed start can be 5.5 h off the wall clock.

11. **Transient lock timeouts surface as red "SAVE FAILED"** (low).
    - `hal_sports_sd_lock` waits only 50 ms (`board.c:94`) while the sports writer holds the same mutex across `fsync` once per second.
    - A timeout (`WORKOUT_IO_LOCK`, `workout.c:752`) fails the ack, so the summary immediately says "SAVE FAILED / RETRY" (`workout.c:693-697`, `fit_workout.c:396`) although the writer auto-retries after 5 s.

12. **`WORKOUT_REJECT_HR_STALE` is unreachable on the device** (low).
    - `hal_hr_get` zeroes any reading older than 2 s (`ext_hr.c:351`), while the workout's staleness limit is 3 s (`workout.c:322`).
    - A stalled HR stream is therefore reported as `HR_CONTACT` in the diagnostics.

13. **Unverified or optional hardware assumption for HR** (low).
    - Without the strap module there is no HR at all and every sample is `HR_MISSING`.
    - Wrist-side PPG during racket sports is motion-corrupted, so HR coverage should be expected to be poor.
    - The summary line only says "HR --".

14. **Docs are stale** (low).
    - `docs/SPORT_MODE.md` still describes six profiles, "no calorie claims", and a six-sport selector. The code has 40 profiles and an energy estimate. `docs/SPORTS_UPDATE.md` describes it correctly.
    - `docs/SPORT_MODE.md:84-90` is the correct pointer for exports.

15. **No route recording or phone sync** (low).
    - Outdoor workouts keep only distance and speed, with no GPX. The separate `svc_track` GPX recorder (`/sdcard/watch/tracks/NNNN.gpx`) is not linked to a session.
    - Workouts are not pushed to the phone (no `gb.c` path); files are reachable only through `sd_xfer`/`sdsync.py`.
