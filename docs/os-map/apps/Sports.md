---
title: "Sports"
id: app-sports
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Sports

## Summary

Sports is the NOS workout UI (`scr_workout`, title "SPORTS", `nos/apps/fit_workout.c:1066`). The recorder is `nos/workout.c`. The app only reads snapshots from it and sends it commands; it never touches sensors or SD directly (`nos/apps/fit_workout.c:1-3`).

- Sessions run in the services hook (`svc_tick` calls `workout_tick` every frame, `nos/svc.c:1321`) and in an SD writer task. Leaving the app, screen-off and AOD do not pause or end a session.
- A session cannot start until the SD history has loaded (`nos/apps/fit_workout.c:551-556`, `nos/workout.c:817`). With no card, or with both history banks damaged, nothing can be recorded, not even time-only.
- Outdoor distance, pace and speed need the external GPS strap, and heart rate needs the MAX30102 strap (`docs/MODULES.md:9-27`). A bare 1.43-B has neither. The app does not check for them; it shows `--` or "NO GPS".
- The strap drivers are documented as "nothing here has run on hardware yet" (`docs/MODULES.md:5`). `docs/SPORT_MODE.md:60` says the implementation is exercised in the desktop simulator and no watch is connected. The status below is from reading source.

Not the same thing as:
- "Sensor logs" (`scr_sports_logs`, `nos/apps/fit_energy.c:285`), which views the all-day sports journal (`nos/sports.c`, `/sdcard/watch/sports`). Sports reads neither.
- The only shared symbol is `hal_sports_sd_lock()` (`nos/sports.h:63`), which `nos/workout.c:591` uses for SD mutual exclusion.

Related system notes: [[Sports and workouts]], [[Health and activity]], [[Services hub]], [[Engine and scheduling]], [[Storage and persistence]], [[Hardware and drivers]], [[Power and battery]], [[Shell and faces]], [[Connectivity and voice]].

## Features

- **Selector** (`choose_draw`, `nos/apps/fit_workout.c:296-308`). Five favourite tiles from `favorites[]` (`fit_workout.c:31-34`): Football, Cricket, Badminton, Run, Hike. Then an "ALL SPORTS" tile (40 profiles, `catalog_screen`) and a "RECENT WORKOUTS" button (`history_screen`). The "40" labels match the 40-row table in `nos/workout.c:56-97`.
- **Ready screen** (`ready_draw`, `fit_workout.c:503-540`). Headline and GPS state for outdoor sports ("GPS READY", "WAITING FOR A GOOD GPS FIX", "GPS NOT AVAILABLE"), the wrist and playing-hand line, START / START ANYWAY / RETRY / LOADING, the "WRIST + HAND SETUP" button, and the crash auto-resume line.
- **Live** (`live_draw`, `fit_workout.c:349-390`). Active time, which excludes pauses. Outdoor: distance, pace or speed, HR, and cadence or average speed. Indoor step sports: HR and steps. Other indoor: HR and wrist movement bursts, or the playing-hand line. Footer shows the lap count and opens details on tap.
- **Controls** (`main_input`, `fit_workout.c:448-486`). LAP (`workout_lap_add`, max 20, `nos/workout.c:853-861`), PAUSE and RESUME. From paused, LEFT goes to FINISH, which asks for confirmation (`finish_screen`, `fit_workout.c:805-825`).
- **Summary** (`summary_draw`, `fit_workout.c:391-419`). Status line: "SAVING TO SD", "SAVED TO SD", "SAVE FAILED / RETRY", or "INSERT ORIGINAL SD / RETRY". Shows active time, distance and GPS average (outdoor) or total duration (indoor), and the average-HR line. Buttons: DETAILS plus DONE or CLOSE, or RETRY plus DETAILS after a failed save.
- **Details** (`detail_enter`, `fit_workout.c:955-1040`). Up to 48 rows: session status, active and total time, accepted GPS distance, average speed or pace, GPS coverage, average and peak HR, HR coverage, placement and frozen wrist source, playing or bowling hand, sensor-location note, wrist bursts and movement time, quality-gate rejections, energy (only if body mass is set), crash recovery count, steps, UTC start, and each lap.
- **History** (`history_refresh`, `fit_workout.c:863-891`). Last 12 sessions with SAVED, SAVING or UNSAVED, duration plus distance or lap count, and UTC date.
- **Setup** (`setup_text` / `setup_tap`, `fit_workout.c:621-680`). Watch wrist, dominant hand, playing hand (racket, cricket batting, volleyball), bowling hand (cricket only), crash auto-resume toggle, body mass (`mass_screen`), and guided wrist detection with left and right learn references (`wrist_screen`, labelled "experimental").
- **Profiles** (`nos/workout.c:56-97`, `nos/workout.h:38-43`). Each profile sets outdoor (GNSS) use, step use, speed display, family, and a reference MET value. Examples: RUN is outdoor and uses steps; BADMINTON is indoor with no steps; TREADMILL is indoor with steps; CRICKET is outdoor and uses the playing and bowling hand settings.

## Reads

Service getters:
- `workout_get()`, `nos/workout.c:237`. Live `current` snapshot, read on every frame (`fit_workout.c:245`, `:430`, `:588`).
- `workout_storage_get()`, `nos/workout.c:906`. Ready, available, dirty, busy, corrupt, foreign, uncertain, errors, worker_failed (`fit_workout.c:246`, `:909`).
- `workout_profile_get()`, `nos/workout.c:235` (`fit_workout.c:335`).
- `workout_history_count()` and `workout_history_get()`, `nos/workout.c:899-905`. RAM copy only (`fit_workout.c:867-871`).
- `wrist_detect_get()`, `nos/wrist_detect.c`, called from `fit_workout.c:737` and `:798`.
- `prefs_wrist_source()` and `prefs_wrist_revision()`, `nos/prefs.c:328-329`, used at `fit_workout.c:111` and `:114`.
- Engine helpers: `nos_revealed()`, `nos_transitioning()`, `nos_t`, `nos_touch_travel()`.

Preferences (`nos_prefs`, `nos/prefs.h:17`, `:28`, `:36-41`):
- `units_metric`: display units only, stored session data stays metric (`fit_workout.c:247`, `:865`, `:960`).
- `wrist_left`: `fit_workout.c:105`, `:532`, `:624`.
- `sports_dominant`: `fit_workout.c:106`.
- `sports_racket`, `sports_batting`, `sports_volleyball`: via `playing_pref()`, `fit_workout.c:91-96`.
- `sports_bowling`: `fit_workout.c:108`.
- `sports_auto_resume`: `fit_workout.c:109`, `:539`, `:630`.
- `sports_mass_kg`: `fit_workout.c:110`, `:633`.
- `wrist_refs[0..3]`: `fit_workout.c:637-638`.

HAL and board calls:
- `hal_epoch_seconds()`, `nos/workout.c:209`. UTC start and end; zero if the clock is before 2020 (`workout.c:207-211`).
- `hal_millis()`, `fit_workout.c:890` and `:900`, for the history refresh stamp.
- `hal_activity_get()`, `nos/workout.c:283` and `:289`. All-day pedometer count (the same counter Activity uses). Delta-sampled for step sports only (`workout.c:286-313`).
- `hal_hr_get()` and `hal_hr_request(HAL_HR_OWNER_WORKOUT, …)`, `nos/workout.c:316-318`, `:841`, `:870`, `:889`. Strap MAX30102.
- `hal_gps_enable()` and `hal_gps_get()`, `nos/workout.c:389-390`, `:705`, `:829`, `:850`. Strap u-blox M10 on UART1.
- `hal_sports_sd_lock()` and `hal_sports_sd_unlock()`, `nos/workout.c:591` and `:751`. Device implementation `firmware/main/board.c:93-98`, which returns 0 when no card is mounted.
- `hal_storage_get()`, `nos/workout.c:589`. Host builds only.
- Not read: `hal_ext_present()`. Sports never checks module presence; it infers absence from getter results (grep of `fit_workout.c` finds no `hal_ext_present`, `hal_gps` or `hal_hr` use).

Diagnostics: `diag_log_name_id()` (`fit_workout.c:432`, `:443`, `:550`, `:676`, `:788`) for the action names.

## Writes

Recorder commands, all through `nos/workout.h` (`nos/apps/fit_workout.c`):
- `workout_setup_set()` and `workout_placement_revision_set()` in `configure_session()` (`fit_workout.c:102-115`). Called from `ready_enter` (`:491`), `ready_input` (`:557`), `setup_tap` (`:674`), `wrist_input` (`:786`) and `mass_input` (`:724`).
- `workout_prepare(sport)` and `workout_unprepare()` (`fit_workout.c:492`, `:495`, `:499`). These keep GPS sampled on the ready screen (`nos/workout.c:796-798`, `:807-811`).
- `workout_start()` (`fit_workout.c:558`).
- `workout_pause()`, `workout_resume()`, `workout_lap_add()`, `workout_finish()` via `session_action()` (`fit_workout.c:428-439`, called at `:468-473` and `:821`).
- `workout_retry_save()` via `session_retry()` (`fit_workout.c:440-446`). Called from RETRY (`:477`) and from tapping the empty history list (`:923-925`).
- `workout_discard()`: no UI caller. The only call is in the simulator (`sim/nos_sim.c:966`).

Preferences (saved with `prefs_save()`, a 2 s debounce, `nos/prefs.h:55`):
- `setup_tap` (`fit_workout.c:655-680`):
  - `prefs_set_wrist(!wrist_left, PREFS_WRIST_SOURCE_MANUAL)` (`:660`). This is the watch-wide wrist setting, also read by Settings (`nos/apps/set_settings.c:446`, `:1743`) and by the wake service (`firmware/main/nos_main.c:944-951`).
  - `sports_dominant` (`:661`), the playing-hand field (`:664`), `sports_bowling` (`:667`), `sports_auto_resume` (`:668`), then `prefs_save()` (`:674`).
- `mass_input` (`fit_workout.c:711-727`): `sports_mass_kg` (`:723`), then `prefs_save()`.
- `wrist_input` (`fit_workout.c:773-801`): `prefs_set_wrist(candidate == LEFT, GUIDED)` (`:780`), `wrist_refs[0..3]` (`:783-784`), then `prefs_save()` (`:786`).

Other:
- `wrist_detect_begin()` and `wrist_detect_cancel()` (`fit_workout.c:796`, `:733`). Internal RAM state only.
- `nos_keep_awake()` while wrist capture runs (`fit_workout.c:738`).
- Diagnostic action records: `diag_action_request/context/result` (`fit_workout.c:431-437`, `:442-445`, `:549-559`, `:675-678`, `:787-797`). The recorder adds its own events (`nos/workout.c:220`, `:233`, `:707`, `:830`, `:842`, `:851`, `:871`). These go to the diagnostics log on SD (`nos/diag_log.c:24`, `:395`).
- RAM UI state: `W` (`fit_workout.c:36-45`), including `dismissed_id` (`:39`). `history_state` and `detail_state` are allocated with `nos_mem_alloc` on first use (`fit_workout.c:312-316`, `:947-951`). This file never frees them.

## Background behaviour

What runs, and who runs it:

- **Recorder tick.** `workout_tick()` (`nos/workout.c:799-812`) runs from `svc_tick` (`nos/svc.c:1321`), reached through `nos_bg_tick` (`nos/apps/registry.c:62-71`, `nos/nos.c:2341`). Its rate depends on power state:
  - Screen on: every rendered frame (40 Hz nominal, `nos/render_config.h:8`; up to 60 Hz present, `:13`).
  - AOD or sensor-idle (`PWR_AOD` / `PWR_SENSOR`): once per second through `nos_frame_run` (`firmware/main/nos_main.c:2106-2117`). With `sensor_idle` on by default (`nos/prefs.c:159`), the idle target can be PWR_SENSOR (80 MHz, 1 Hz display, `nos/prefs.h:33`) rather than PWR_OFF (`firmware/main/nos_main.c:2085`).
  - Screen off (`PWR_OFF`): once per second from the power loop (`firmware/main/nos_main.c:2094`).
  - Not called when `mutate == 0`, which is during an OTA activity or the final shutdown phase (`nos_main.c:1984-2026`).
- **Sampling** at 1 Hz (`SAMPLE_MS`, `nos/workout.c:46`, `:805`): step delta, HR, GPS and averages (`metrics()`, `workout.c:461-467`). Active time advances on every tick (`advance_time`, `workout.c:268-280`).
- **Checkpoints.** Every state change (start, pause, resume, lap, finish, discard) requests one (`nos/workout.c:830`, `:843`, `:851`, `:860`, `:877`, `:890`). While running, one is taken every 30 s (`CHECKPOINT_MS`, `workout.c:47`, `:806`). A failed write retries every 5 s (`RETRY_MS`, `workout.c:48`, `:726`).
- **SD writer.** Task `workout_sd`, priority 2, 6 KB stack, PSRAM-capable (`nos/workout.c:763-778`). Created lazily through `writer_ensure` (`nos/workout.c:766`). Polls every 500 ms (`workout.c:764`). Holds the SD lock only while it reads or writes (`workout.c:731-760`). The UI never takes the lock.
- **IMU feed.** Task `board-sense`, priority 2, unpinned, 4 KB static stack (`firmware/main/board.c:1036`). It polls the QMI8658 with three register reads per roughly 20 ms cycle (`board.c:967-1026`, reads at `:985` and in `knock_poll`, `:316-320`). The board.c comment says the chip runs at 500 Hz; the rate that reaches Sports is about 150 Hz nominal. `imu_take` (`board.c:298-305`) feeds `workout_motion_sample` (`:302`) and `wrist_detect_feed` (`:303`).
  - Motion is counted only while a session is RUNNING (`nos/workout.c:135-139`, `:158-159`).
  - Entry threshold: |a-1| > 0.16 g or gyro > 55 dps. Once active, the thresholds drop to 0.10 g or 35 dps (`nos/workout_motion.c:33`).
  - Entry hold 300 ms, exit hold 600 ms (`workout_motion.c:39`, `:49`). Samples more than 250 ms apart are gaps (`:21-24`).
  - These are "movement episodes", not repetitions (`fit_workout.c:385`, `:383`).
- **GPS (strap).** `hal_gps_enable(1)` on start and resume, and on every outdoor sample (`nos/workout.c:829`, `:850`, `:389`). The ext task (priority 0, `docs/MODULES.md:13`) services the UART. The stream stops by itself after 8 s without a `hal_gps_get()` call (`firmware/main/ext/ext_gps.c:13-14`; `docs/MODULES.md:29-30`). Pause makes no GPS calls, so GPS stops about 8 s into a pause.
- **HR (strap).** Owner lease `HAL_HR_OWNER_WORKOUT` is taken on start and resume and released on pause, finish and discard (`nos/workout.c:830`, `:841`, `:851`, `:870`, `:889`). Each second's sample renews it. The module also idles out when not read (`firmware/main/ext/ext_hr.c:238`, `:330-343`).
- **Jobs tile.** `background_tick` publishes the BG_WORKOUT job (active time, session steps, energy estimate), at most once per second (`nos/background.c:127-158`; job entry at `:21-35`; `nos/background.h:9`).
- **Home chip.** `sys_rec_state()` shows "SPORT mm:ss" or "PAUSE mm:ss" while RUNNING or PAUSED (`nos/apps/sys_home.c:163-170`). The same chip appears on the control sheet (`nos/apps/sys_control.c:284`, `:352-357`).

What stops a session:
- Nothing automatic. There is no auto-pause, auto-lap or stop detection (`docs/SPORT_MODE.md`, "There is no automatic pause").
- Restart and deep sleep from the power sheet are refused while the session is RUNNING, PAUSED, or FINISHED but unsaved. The toast says "SAVE RECORDING FIRST" (`firmware/main/nos_main.c:741-744`, `:757`). Display sleep is not blocked (`nos_main.c:751-754`).
- OTA is refused while recording (`firmware/main/ota.c:57-63`, `:297`).

Keep-awake:
- No Sports screen sets `SCR_KEEPAWAKE` (flags at `fit_workout.c:1058-1066` are `SCR_LIVE_RENDER` only). Only the wrist-capture screen calls `nos_keep_awake()` (`fit_workout.c:738`). The auto-sleep check in `nos_main.c:2080` honours only `SCR_KEEPAWAKE` on the top screen.
- The screen therefore times out after `sleep_s`, default 15 s (`nos/prefs.c:149`), minimum 5 s (`prefs.c:168`).

## Persistence

- **Session journal, SD.** `/sdcard/watch/workouts/workouts.0.bin` and `workouts.1.bin` (`nos/workout.c:115`, `:522-523`). Host builds use `build/nos_workouts` (`workout.c:119`).
- **Format.** `disk_record`, 4304 bytes, magic `0x574f5331`, version 3, FNV-1a checksum at offset 4296 (`workout.c:44-45`, `:98-111`; `nos/workout.h:11-22`). Holds the current session, the 12 newest snapshots (328 bytes each, `workout.c:106`), `next_id`, and `hr_integral`.
- **Write.** Alternating banks; the higher `sequence` is newest (`workout.c:566-575`, `:620-623`). Each write is followed by fflush, fsync (`SYNC`), close, and a readback compare (`workout.c:624-643`). The UI marks SAVED only when the writer's ack matches the current revision (`workout.c:685-698`).
- **Foreign-card guard.** Known and candidate bank hashes stop a different card's journal from being overwritten (`workout.c:593-608`). Sets `storage.foreign`.
- **Corrupt history.** If both banks are invalid, they are preserved and `storage.corrupt` is set. Nothing is overwritten (`workout.c:528-564`, `:720-748`).
- **Restore.** Done at boot on the writer. `apply_storage` (`workout.c:658-710`) restores RUNNING as RUNNING if `setup.auto_resume`, otherwise as PAUSED. It marks `recovered` and increments `recovery_count` (`workout.c:666-681`).
- **NVS.** None. Neither `nos/workout.c` nor `nos/apps/fit_workout.c` uses NVS or `hal_store`.
- **Preferences.** `sports_*` and `wrist_*` live in `nos_prefs_t` (`nos/prefs.h:36-41`). Saved with the 2 s debounce to `/watch/settings.{0,1}.bin` (`nos/prefs_disk.c:32`), with the NVS fallback described in `nos/prefs.c`.
- **Not persisted.** `W.dismissed_id`, the selected sport, the wrist-learn mode, the mass draft, and the history and detail UI caches. The dismissed flag is RAM only (see Gaps).
- **Diagnostics.** `/sdcard/watch/diagnostics/diag-NN.bin` (`nos/diag_log.c:24`, `:395`).

## Navigation

Opens:
- `ready_screen` "SPORT READY" (`fit_workout.c:321-323`, `:579`).
- `catalog_screen` "ALL SPORTS" (`fit_workout.c:321`).
- `history_screen` "WORKOUT HISTORY" (`fit_workout.c:317`).
- `detail_screen` "SESSION DETAILS" (`fit_workout.c:465`, `:478-479`, `:860`, `:953`).
- `finish_screen` "FINISH SESSION" (`fit_workout.c:472`).
- `setup_screen` "SESSION SETUP" (`fit_workout.c:547`).
- `mass_screen` "BODY MASS" (`fit_workout.c:669`).
- `wrist_screen` "DETECT WRIST" (`fit_workout.c:671-672`).

Opened from:
- Launcher: "Sports" entry (`nos/apps/registry.c:10`).
- Home record chip (`nos/apps/sys_home.c:174`, `:519`; kind 4 maps to `scr_workout`).
- Control-sheet record chip (`nos/apps/sys_control.c:352-357`).
- Jobs app, Sports session row (`nos/apps/sys_background.c:210-218`, `:245`).

Returns:
- START success pops back to the Sports main screen, which shows the live view (`fit_workout.c:560`).
- The summary's CLOSE pops when the save is not complete. DONE clears the summary for this session (`fit_workout.c:481-482`).

## Status

**Partial.** The recorder, the SD checkpoint path, the save acknowledgements and the UI are all present and consistent in source. Gaps: the SD-only start, no discard path, no keep-awake, and GPS and HR that depend on strap hardware whose on-device behaviour is not verified.

## Gaps

1. **High. No start without a readable SD history.** `ready_input` refuses to start unless `storage.ready` (`nos/apps/fit_workout.c:551-556`). `ready_draw` shows "SD card unavailable" or "History unreadable" (`fit_workout.c:509-512`, `:536`). `workout_start` requires `storage.ready` (`nos/workout.c:817`). `storage.ready` is set in `apply_storage` after a successful load (`nos/workout.c:683`; the `loaded` flag is set at `:758`). No card means `hal_sports_sd_lock` returns 0 (`firmware/main/board.c:93-96`). Both banks damaged leaves `ok` false (`workout.c:747`). Impact: a watch with no card, or a damaged history, cannot record at all, even time-only. This is intended (`docs/SPORT_MODE.md:6-8`), but it has no RAM-only fallback.
2. **High. No way to discard an unsaved or stuck session.** `workout_discard()` has no UI caller (only `sim/nos_sim.c:966`). A FAILED or PENDING save then:
   - blocks new starts (`nos/workout.c:817-819`; `fit_workout.c:557-561` shows "FINISH SAVING CURRENT SESSION");
   - blocks OTA (`firmware/main/ota.c:62-63`);
   - blocks restart and deep sleep (`firmware/main/nos_main.c:741-744`).
   The only exit is a verified write (`workout.c:685-698`), and with a swapped card that needs the original card (`fit_workout.c:395`, `:515-517`; `workout.c:603-608`). A dead card can therefore hold the device's power-off and updates.
3. **Medium. The screen times out during a session.** No Sports screen sets `SCR_KEEPAWAKE` (`fit_workout.c:1058-1066`). Only wrist capture keeps the watch awake (`fit_workout.c:738`). Default `sleep_s` is 15 (`nos/prefs.c:149`) and AOD is off by default (`prefs.c:144`). The user must raise the wrist or tap to read the live screen mid-run.
4. **Medium. No automatic pause, lap, interval, target or alert.** Manual laps only, capped at 20 (`nos/workout.h:9`; `nos/workout.c:855-861`). There are no HR-zone, pace, time or distance alerts. Active time keeps counting while the user is stopped until PAUSE is tapped (`docs/SPORT_MODE.md`).
5. **Medium. A session restored after a reboot keeps running by default.** `sports_auto_resume` defaults to 1 (`nos/prefs.c:160`). The restore path keeps RUNNING (`nos/workout.c:666-667`), and `advance_time` counts from the boot (`workout.c:268-280`). Active time therefore keeps counting after an unexpected reboot until the user pauses or finishes. The live status says "RECOVERED / PAUSED" only when the restored session is paused (`fit_workout.c:277`), so a restored running session shows no recovery label on the live screen; the count appears only in DETAILS (`fit_workout.c:1020-1021`). The setup row reads "Automatic / excludes downtime" (`fit_workout.c:630`).
6. **Medium. No strap-hardware fallback messaging.** GPS and HR come only from the external strap (`docs/MODULES.md:9-27`). Sports never calls `hal_ext_present()`. Missing GPS shows "GPS NOT AVAILABLE" (`nos/apps/fit_workout.c:519-521`) or "RECORDING / NO GPS" (`nos/apps/fit_workout.c:281`, `:374`). Missing HR just shows `--` (`fit_workout.c:261-262`). Nothing tells the user a strap is not fitted. Impact: a bare 1.43-B gives time-only sessions with no explanation.
7. **Medium. No route or GPX.** Only the Navigate track recorder writes GPX (`nos/svc_track.c:255`; `nos/svc_track.h:1-12`). Sports stores a session summary plus at most 20 laps (`nos/workout.h:9`; `nos/workout.c:859`), with no track points, and `docs/SPORT_MODE.md:30` describes the same model. Outdoor sessions have no map or route.
8. **Medium. Calories are hidden by default.** Body mass defaults to 0 and is zeroed on migration (`nos/prefs.c:146-160`, `:175`). Energy appears only in DETAILS, and only when mass is set (`fit_workout.c:1013-1019`). Live and summary screens show no kcal (`fit_workout.c:349-419`).
9. **Medium. History is 12 sessions with no delete, export or phone sync.** Older sessions roll off silently on the 13th save (`nos/workout.h:10`; `nos/workout.c:875-876`). Export is PC-only, through `tools/workouts_export.py` (`docs/SPORT_MODE.md:80-94`). `nos/gb.c` has no workout reference, so nothing reaches the phone.
10. **Medium. Sports changes the watch-wide wrist.** "WATCH WRIST" (`fit_workout.c:624`, `:660`) writes the same preference that Settings and the wake service use (`nos/apps/set_settings.c:446`, `:1743`; `firmware/main/nos_main.c:944-951`). Changing it in Sports silently changes raise-to-wake.
11. **Medium-low. Pause releases GPS, and resume reacquires.** After 8 s of pause the GPS stops (`firmware/main/ext/ext_gps.c:13-14`; pause makes no GPS call, `nos/workout.c:837-844`; resume re-enables it at `:850`). Resume needs a hot start of 1-2 s with backup power, or a cold start of about 30 s otherwise (`docs/MODULES.md:122`). Distance and pace stay blank in the meantime, and the gap is not counted (`nos/workout.c:426-429`, `:449-453`). Power-positive, but the first minute after resume can show no pace.
12. **Low. No stroke or rep counting for racket and team sports.** The number shown is wrist-movement episodes only, labelled "BURSTS / EST." and "WRIST MOVEMENT / NOT REPS" (`fit_workout.c:378-385`; `nos/workout_motion.c:31-32`).
13. **Low. Wrist detection is experimental.** It cannot distinguish a 180-degree strap reversal (`nos/wrist_detect.h:4-7`; `fit_workout.c:636`).
14. **Low. Misleading toasts.** "LAP LIMIT REACHED" appears for any rejected mark, including a duplicate-time mark (`fit_workout.c:469`; `nos/workout.c:857-858`). "FINISH SAVING CURRENT SESSION" appears when OTA or shutdown blocks the start (`fit_workout.c:558-561`; `nos/workout.c:815`).
15. **Low. Dismissal of a finished session is RAM only.** `W.dismissed_id` (`fit_workout.c:39`, `:481`) is not saved. After a reboot, the last saved summary reopens (`view_for_session`, `fit_workout.c:325-331`) until DONE is tapped again.
16. **Low. DETAILS for a running session is a frozen copy** taken when opened (`fit_workout.c:952`), labelled "AT OPEN / SESSION STILL RUNNING" (`fit_workout.c:1049-1050`).
17. **Low. No charts, HR zones or per-minute data.** Live and summary screens show scalars only (`fit_workout.c:349-419`).
18. **Low. Dead setup rows.** "Not used for this sport" and "Cricket only" rows accept taps and do nothing (`fit_workout.c:628-629`, `:663`, `:666`).

19. **Low. Docs and code disagree on calories.** `docs/SPORT_MODE.md:22-23` says there are no calorie claims. The code shows a gross kcal figure in DETAILS, labelled "ENERGY / GROSS ESTIMATE" and "ENERGY ASSUMPTION / NOT MEASURED" (`nos/apps/fit_workout.c:1013-1017`), and an "Energy estimate" in the Jobs tile (`nos/background.c:29`). The figure is MET x 3.5 x mass / 200 per minute of active time (`nos/workout.c:276-278`). The UI labels it an estimate, but the doc says the feature makes no calorie claims.
