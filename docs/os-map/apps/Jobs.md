---
title: "Jobs"
id: app-jobs
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Jobs

Launcher label **Jobs** (`nos/apps/registry.c:12`). Overview title **BACKGROUND JOBS** (`nos/apps/sys_background.c:84`); detail title is each job's own title, status line **JOB DETAILS** screen (`nos/apps/sys_background.c:259`). Source: `nos/apps/sys_background.c` (260 lines), collector `nos/background.c` (215 lines), API `nos/background.h` (53 lines). Target: 1.43-B.

## Summary

Jobs is a read-only status board for 13 background "jobs": motion, steps, calorie estimate, battery, SD sensor journal, system trace, IMU recording, sports session, and five optional strap modules (heart, GPS, climate, light, compass). Each job has a state, a last-update time, an update count, three values, and a 2-minute RAM history. The screen changes nothing. It writes no NVS keys, no prefs and no SD files.

The collector is not in the screen. `background_tick()` runs from the engine service hook (`nos/apps/registry.c:62-66`, `nos/background.c:160-186`) and the screen only reads cached snapshots (`nos/apps/sys_background.c:62-75`). Event-driven jobs (motion, heart, GPS, climate, light, compass) are fed by sensor producers through `sports_observe()` (`nos/background.c:124`, `nos/background.c:90-116`).

Doc check against `docs/BACKGROUND_JOBS.md`: the described screens, tabs, record links, RAM-only history and "no sensor enable" rule match the code. Not verifiable from source: the "installed on COM3 on 2026-10-22" line and the host test and frame counts. `CLAUDE.md` records COM3 firmware identity as unconfirmed on 2026-10-23. The battery sentence is broader than the code (see Gaps). "Power-off stops sampling" is true for deep sleep and shutdown (`firmware/main/nos_main.c:811`) but not for the display-off state `PWR_OFF`, which keeps ticking at 1 Hz (`firmware/main/nos_main.c:2091-2094`).

## Features

- **Overview (4 rows per page, 13 jobs, 4 pages)**: state dot (red for ERROR or STALE, white for RUNNING, grey otherwise), job name, state name, and the first metric value or `-- unit` when invalid (`nos/apps/sys_background.c:89-98`).
- **Header counter**: "N RUNNING / M NEED ATTENTION", where attention means ERROR or STALE only. NO MODULE, WAITING and IDLE are not counted (`nos/apps/sys_background.c:86-88`).
- **Paging**: `<` and `>` buttons wrap around. No swipe paging (`nos/apps/sys_background.c:99-101`, `203-206`).
- **Detail status line**: state, "LAST Ns", and the update count labelled CHECKS for steps, calories, trace and workout, otherwise UPDATES. Shows "NO MODULE / n EARLIER UPDATES" or "NO READING YET" when relevant (`nos/apps/sys_background.c:177-185`).
- **GRAPH tab**: a 2-minute line plot at one point per second. Tap the value to cycle metrics (empty metric names are skipped). Tap the plot to freeze on the nearest point, showing "UP hh:mm:ss"; tap again to go live. The footer shows the range, the motion axes, or ERRORS and DROPPED for journal, trace and IMU, otherwise the job's about text (`nos/apps/sys_background.c:104-149`, `nos/apps/sys_background.c:222-245`).
- **OPEN RECORDS button**: pushes a destination screen chosen by job (`nos/apps/sys_background.c:148`, `210-221`, `245`).
- **READINGS tab**: up to 120 one-second summaries, newest first, 4 per page, NEWER/OLDER paging, RESUME LIVE (`nos/apps/sys_background.c:150-172`, `247-256`).
- **EVENTS tab**: up to 16 state transitions or error/drop counter changes, newest first, paged (`nos/background.c:67-75`, `nos/apps/sys_background.c:150-172`).
- **Haptic feedback** on taps and page turns (`nos/apps/sys_background.c:199`, `206`, `228`).
- **Serial `jobs` command**: prints one JOB line per id from `background_status_get`, without waking the display (`firmware/main/nos_main.c:1193-1204`, quiet list at `firmware/main/nos_main.c:2001`). `tools/watchctl.py` lists `jobs` (`tools/watchctl.py:44`).

## Jobs table (what each job reads)

| id | Job | Metrics (units) | Source | Module gate | Stale after | On demand |
|---|---|---|---|---|---|---|
| 0 | Motion sensor | Acceleration g (magnitude), Rotation dps (magnitude), Z accel g | `sports_observe` SP_IMU per IMU read | none | 2000 ms | no |
| 1 | Step tracking | Steps today, Active minutes, Walking 0/1 | `hal_activity_get` + `svc_activity()` each tick | none | 3000 ms | no |
| 2 | Calorie estimate | Activity energy kcal, steps, active min | `svc_activity()` kcal (steps x 0.04) | none | 3000 ms | no |
| 3 | Battery sampling | Supply voltage mV, level %, external power 0/1 | `hal_power_get` | none | 4000 ms | no |
| 4 | SD sensor journal | Pending, written, write errors | `sports_get_status` (drain heartbeat) | none | 10000 ms | no |
| 5 | System trace | Pending, written, errors | `diag_log_get` | none (stale 0, never stale) | - | no |
| 6 | IMU recording | Samples, elapsed s, missed | `hal_imu_record_get` | none | 3000 ms | yes |
| 7 | Sports session | Active s, session steps, energy kcal est | `workout_get` | none | 3000 ms | yes |
| 8 | Heart sampling | Heart rate bpm, signal quality %, SpO2 % | `sports_observe` SP_HEART | EXT_HR | 4000 ms | yes |
| 9 | GPS receiver | Satellites, speed m/s, HDOP | `sports_observe` SP_GPS | EXT_GPS | 5000 ms | yes |
| 10 | Climate sensor | Temperature C, humidity %, pressure hPa | `sports_observe` SP_ENV | EXT_ENV | 7000 ms | no |
| 11 | Light sensor | Illuminance lux, proximity 0-255 | `sports_observe` SP_LIGHT | EXT_LIGHT | 3000 ms | no |
| 12 | Compass sensor | Heading deg, field uT, calibrated 0/1 | `sports_observe` SP_MAG | EXT_MAG | 3000 ms | yes |

Source: job table `nos/background.c:21-35`, `background_info` `nos/background.h:17-21`. Stale and state rules are in `nos/background.c:160-186`.

## Reads

Per-second collector reads (`nos/background.c:127-159`, `nos/background.c:160-186`):

- `hal_millis()`: `core/hal.h:18`; device `firmware/main/hal_esp32.c:111-113`.
- `hal_activity_get()`: `core/hal.h:287`; device `firmware/main/board.c:495-505`. Valid only with an IMU sample under 250 ms old, calendar ready and prefs not loading.
- `svc_activity()`: `nos/svc.h:102`, `nos/svc.c:438`. Fields used: steps, kcal, active_min. kcal = steps x 0.04 (`nos/svc.c:46`).
- `hal_power_get()`: `core/hal.h:33-41`; device `firmware/main/hal_esp32.c:378-389`. Sampled at 1 Hz by the board task (`firmware/main/board.c:1021`, `firmware/main/hal_esp32.c:316`). Reports sample_valid=0 after 3 s (`firmware/main/hal_esp32.c:385`).
- `sports_get_status()`: `nos/sports.c:213-214`. Fields: written, dropped, errors, queued, available, last_service_ms.
- `diag_log_get()`: `nos/diag_log.c:326-327`. Fields: queued, written, errors, dropped, ready, available.
- `hal_imu_record_get()`: `core/hal.h:173`; `nos/imu_rec.c:163-164`. Fields: state, samples, elapsed_ms, missed, errors, dropped. The `message` field is not read.
- `workout_get()`: `nos/workout.h:127`, `nos/workout.c:237`. Fields: state, save_state, elapsed_ms, steps, energy_kcal_est.
- `hal_ext_present()`: `nos/hal_ext.h:64`, `firmware/main/ext/ext_sys.c:197-199`.

Pushed observations (no polling, `nos/background.c:90-116`, `nos/sports.c:145-151`): SP_IMU (board task), SP_HEART (`firmware/main/ext/ext_hr.c:296`), SP_GPS (`firmware/main/ext/ext_gps.c:195`), SP_ENV (`firmware/main/ext/ext_env.c:174`), SP_LIGHT (`firmware/main/ext/ext_light.c:232`), SP_MAG (`firmware/main/ext/ext_mag.c:187`). Observers run even when the journal queue is full (`nos/sports.c:142`, `150`).

Screen-side reads (`nos/apps/sys_background.c:62-75`): `background_summary`, `background_status_get` (all 13, once per second), `background_get` (selected job, once per second, unless frozen), `background_job`, `background_state_name`. Input: touch taps only (`nos/apps/sys_background.c:193-258`).

## Writes

- Collector RAM: `data` (13 `job_data`, PSRAM, owner `background.history`, `nos/background.c:120`), plus `recorder_state`, `last_tick`, `ticked` (`nos/background.c:44-47`). Writes go through `publish`, `state`, `point` and `counters` (`nos/background.c:56-89`).
- One-time registration at boot: `sports_observe(observe)` writes the observer slot in `nos/sports.c:152-153`, from `background_init()` (`nos/background.c:117-126`).
- Screen RAM `s` (PSRAM, owner `background.ui`, `nos/apps/sys_background.c:78`): page, job, tab, metric, offset, selected, frozen, snapshot, latest[13], x/y arrays, hash.
- Screen stack and feedback: `nos_push` for detail and records screens (`nos/apps/sys_background.c:199`, `245`), `nos_feedback` (`:199`, `206`, `228`), `nos_invalidate` (`:206`, `228`, `233`, `242`, `253`).
- Not written: no `hal_store_*` or `prefs_*` calls in either file. No sensor enables. `hal_imu_record_start`, workout, sports and diag writers are not called.

## Background behaviour

What keeps running, and who runs it:

| Context | Runs | Rate | Display ON | Display OFF (PWR_OFF) | AOD / SENSOR | Deep sleep / shutdown |
|---|---|---|---|---|---|---|
| UI task `app_main`, `services_tick` (`nos/apps/registry.c:62-66`) | `background_tick()`, throttled to 1 Hz internally (`nos/background.c:163`) | per frame, 1 Hz effective | yes, `nos/nos.c:2341` (skipped in readonly frames, `nos/nos.c:2322`) | yes, 1 Hz, only when `mutate` (`firmware/main/nos_main.c:2094`) | yes, 1 Hz `nos_frame_run` (`firmware/main/nos_main.c:2114-2117`) | no |
| `board-sense` static task, prio 2 (`firmware/main/board.c:967`, `1028`) | IMU read calls `sports_imu` (`firmware/main/board.c:300`) | 20 ms loop, knock polls at 8 and 16 ms (`firmware/main/board.c:1021-1025`) | yes | yes until board parks (`s_stopping`, `firmware/main/board.c:972-977`) | yes | no |
| `ext` task, prio 0 (`firmware/main/ext/ext_sys.c:4-12`) | HR, GPS, ENV, LIGHT, MAG producers | per module | yes | yes | yes | no |
| `sports_sd` writer, prio 1 (`nos/sports.c:282-297`, `323`) | drain, which advances the SD journal heartbeat | 1 s | yes | yes | yes | no |

- Motion updates arrive on every IMU read, not once per second. Points are stored at most once per second (`nos/background.c:56-66`). The docs quote about 150 updates per second; not re-measured here.
- HR is background-sampled in windows of up to 40 s, every 10 min, first at 2 min after boot (`firmware/main/ext/ext_hr.c:34-35`, `242-251`, `322`).
- Background publishes stop when the 1 Hz loop does not run: OTA readonly frames and shutdown phase 3 (`firmware/main/nos_main.c:1984`, `2006`, `2026`, `2094`).
- Opening Jobs does not wake a sensor or refresh stale data. It only reads cached state (`nos/apps/sys_background.c:76-81`).
- Screen tick: `refresh()` runs at most once per second while the screen is on top (`nos/apps/sys_background.c:66`, `81`).

## Persistence

- Nothing from this app is persisted. History is RAM only: about 36 KB PSRAM for `background.history` (13 x 2,792 B, estimated from struct layout), plus a few KB for `background.ui`. Reboot or deep sleep clears it. The screen says "RAM HISTORY / RESTART CLEARS" (`nos/apps/sys_background.c:140`).
- No NVS keys and no SD paths are touched by Jobs.
- The sensor records it observes persist through their own journals. The sports journal writes `/sdcard/watch/sports/YYYYMMDD-events.spt` and `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt` (`nos/sports.c:25`, `215-219`). The raw types (IMU, ENV, LIGHT, MAG, HEART, GPS) go to the hourly sensors file (`nos/sports.c:56`). Diagnostics use `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`). Workouts use `/sdcard/watch/workouts` (`nos/workout.c:115`).
- Sports NVS keys `sportsfood` and `sportsmarks` (`nos/sports.c:310`, `313`) belong to the sports module, not to Jobs.

## Navigation

- Opened from the launcher list: `{ "Jobs", DOT_ICON_GAUGE, &scr_background }` (`nos/apps/registry.c:12`). Serial `open` resolves by name through `screen_by_name` (`firmware/main/nos_main.c:924`, `1558`). The simulator lists it (`sim/nos_sim.c:75`).
- No other in-app entry point: `scr_background` is referenced only by the registry, `nos/apps/nos_apps.h:11`, the simulator and `tools/build_identity.py:36`.
- Overview to JOB DETAILS: tap a row (`nos/apps/sys_background.c:193-199`).
- JOB DETAILS to records screen, chosen in `records_screen()` (`nos/apps/sys_background.c:210-221`):
  - MOTION and IMU_REC to `scr_imu` (`nos/apps/out_imu.c:195`)
  - STEPS to `scr_activity` (`nos/apps/fit_activity.c:547`)
  - CALORIES to `scr_energy` (`nos/apps/fit_energy.c:283`)
  - BATTERY to `scr_battery_history` (`nos/apps/fit_energy.c:284`)
  - TRACE to `scr_developer` (`nos/apps/set_developer.c:143`)
  - WORKOUT to `scr_workout` (`nos/apps/fit_workout.c:1066`)
  - JOURNAL, HEART, GPS, CLIMATE, LIGHT, COMPASS to `scr_sports_logs` (`nos/apps/fit_energy.c:285`)
- Back gesture returns (no SCR_NOBACK on either screen, `nos/apps/sys_background.c:259-260`).
- Related notes: [[Shell and faces]] (launcher entry), [[Engine and scheduling]] (service hook and power states), [[Services hub]] (where `svc_tick` and `background_tick` run).

## Gaps

- **Init failure is silent and permanent (medium).** `background_init()` returns with no log and is never retried if the 13 x 2.8 KB PSRAM allocation fails (`nos/background.c:117-126`). The overview then shows "MONITOR UNAVAILABLE" (`nos/apps/sys_background.c:85`) and the serial `jobs` command prints "ERR background monitor unavailable" (`firmware/main/nos_main.c:1196`) until reboot. The sports observer is also never registered, so motion and strap data stop. See [[Storage and persistence]].
- **Dead on-demand sensors look idle (medium).** On-demand jobs with a present module drop to IDLE, not STALE, when samples stop (`nos/background.c:175-178`). Heart, GPS and compass therefore show IDLE when a module has failed. Module presence is probed once at boot and never cleared for these sensors (`firmware/main/ext/ext_sys.c:169-190`; only audio clears its bits, `firmware/main/ext/ext_audio.c:347`). See [[Hardware and drivers]].
- **Heart is nearly always IDLE on this screen (medium).** HR is sampled in 40 s windows every 10 min (`firmware/main/ext/ext_hr.c:34-35`). The screen keeps 120 s of history (`nos/background.h:14-16`, `nos/background.c:197`). The HR graph is almost always empty, and "OPEN RECORDS" goes to the generic sensor logs, not heart data. See [[Health and activity]].
- **Read-only: no controls and no energy attribution (medium).** Nothing can pause, stop or disable a job or change its rate, and no column shows which job drains power. The only controls are page, tap, metric, freeze and records links (`nos/apps/sys_background.c:193-258`). Battery consumption lives in other apps. See [[Power and battery]].
- **History is RAM only and 2 minutes long (low).** No day or week view and no persistence in this app (`nos/background.h:14-16`, `nos/background.c:197`). Durable history is only in the other apps and the SD journals. See [[Storage and persistence]].
- **Attention is passive (low).** NEED ATTENTION counts only when the screen is open (`nos/apps/sys_background.c:86-88`). The collector never calls notifications or feedback (`nos/background.c`, no such calls). A job going to ERROR or STALE raises no alert.
- **ERROR has no reason text (low).** `background_info` and `background_status` carry no message field (`nos/background.h:17-36`). The IMU recorder's `message` (`core/hal.h:158-164`) and workout save state are not shown. The journal reports only "ERROR" (`nos/background.c:141`). See [[Sports and workouts]].
- **Open-records routing is coarse (low).** Six jobs open the same generic sensor logs (`nos/apps/sys_background.c:219`). TRACE opens Developer tools. No job, date or filter is passed (`nos/apps/sys_background.c:245`). See [[Shell and faces]].
- **Frozen views do not say so (low).** Readings and Events freeze when opened (`nos/apps/sys_background.c:226`), with no live indicator. The "LAST Ns" age also stops advancing while frozen because `snapshot.now` is fixed (`nos/apps/sys_background.c:179-181`).
- **Battery row shows mV, not level (low).** The overview row shows supply voltage (`nos/background.c:25`, `nos/apps/sys_background.c:96`). Battery level is valid only on battery after 10 stable samples (`firmware/main/hal_esp32.c:358-360`) and never on USB (`firmware/main/hal_esp32.c:351`). "External power" is inferred from the USB-serial host connection or a sustained high rail (`firmware/main/hal_esp32.c:323`, `351`), because the B board has no charge-status pin (`CLAUDE.md`, hardware table). See [[Power and battery]].
- **Number formatting is confusing (low).** Values below 100 always get two decimals, so counters show "0.00 records" (`nos/apps/sys_background.c:26-29`). Boolean metrics show "1.00 0 / 1" (`nos/background.c:22-34`, units `0 / 1`).
- **Events ring churns on journal drops (low, code-path inference).** While the queue is full, `dropped` changes every second and each change adds an event (`nos/sports.c:142`, `nos/background.c:70`, `72`). Older entries are pushed out of the 16-slot ring. With no SD card, `drain()` returns before taking records (`nos/sports.c:246`), so the queue fills and drops (`nos/sports.c:134-143`).
- **System trace can never go stale (low).** `stale_ms` is 0 (`nos/background.c:27`), so a stopped trace reporter cannot show STALE.
- **No connectivity or voice jobs (low).** BLE link, Wi-Fi, LTE, NUS and microphone are not monitored. SP_MIC exists (`nos/sports.h:11`) but no job observes it. See [[Connectivity and voice]].
- **Overview is thin (low).** It shows one value per job, no age, no trend, no sort or search, and paging is arrow-only (`nos/apps/sys_background.c:82-103`, `193-206`).
- **Naming and doc drift (low).** The launcher says Jobs, the screen says BACKGROUND JOBS, the doc says Background Jobs, and `tools/build_identity.py:36` says Background jobs. The doc's COM3 install and test counts are not verifiable from source.
