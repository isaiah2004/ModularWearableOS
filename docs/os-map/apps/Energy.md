---
title: "Energy"
id: app-energy
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Energy

Source: `nos/apps/fit_energy.c` (287 lines). One file holds five screens: ENERGY, BATTERY HISTORY, SENSOR LOGS, QUICK REPORT and WRIST TUNING. The data layer is `nos/sports.c`, `nos/tuning.c`, `core/wake_service.c`, `nos/svc.c` and `nos/prefs.c`. Producers are in `firmware/main/board.c` and `firmware/main/hal_esp32.c`. Read-only source review: nothing was built, flashed or run on a board, and the serial port was not used. Evidence is `repo-relative/path:line`.

## Summary

Energy is a viewer over the sports journal plus three small writers. It computes no energy of its own. The ENERGY total is `svc_activity()->kcal`, which is steps × 0.04 kcal (`nos/svc.c:45-46,445`; `nos/apps/fit_energy.c:144`).

The five screens are kinds 0–4 of one state table. `states[5]` holds one `energy_state` per kind, allocated on first use (`fit_energy.c:15-26`, `:42-50`). The global `active` selects the state being drawn, ticked and handled (`fit_energy.c:26-27`). Child screens use cut transitions (`TX_NONE`, `fit_energy.c:2`, `:247`, `:256`, `:274`, `:278`).

- **Reads:** journal history through an async query, the event browser, power, activity, wake-learn state and tuning.
- **Writes:** food and water entries (NVS ring), labelled reports (NVS ring plus journal), and tuning or trial markers (NVS blob plus journal). The only prefs write is the "IDLE 80MHz" toggle.
- **Persistence:** delegated to the sports journal and NVS ([[Storage and persistence]]). The screens keep only RAM view state.
- **Status: partial.** This is a source-level judgement, not a bench result. The defects are in the no-SD path and the food/water ring. See Gaps.

Related: [[Health and activity]], [[Sports and workouts]], [[Power and battery]], [[Services hub]], [[Engine and scheduling]], [[Hardware and drivers]], [[Shell and faces]], [[Connectivity and voice]].

## Features

### ENERGY (kind 0; `draw_history` `fit_energy.c:135-157`, `input` `:220-250`)
- Day navigation at y 85–125 steps back and forward. The forward step stops at today (`:243`).
- A 24-bin bar chart over the chosen window (`:99-125`). The metric row at y 140–180 cycles BURNED / FOOD / WATER (`:244`, `:147`).
- Drag across bars to zoom the window (`:221-228`). A tap at y ≥ 405 resets to the full day (`:250`).
- "+ FOOD" and "+ WATER" open an in-screen stepper: ±50, ±1, +250, clamped to 1–10000 (`:126-134`, `:233-242`). Save calls `sports_intake` (`:238`).
- UNDO removes the latest entry, of either type (`:249`, `sports.c:352-365`).
- The footer reads "ACTIVITY CALORIES ONLY" unless a message is set (`:156`).

### BATTERY HISTORY (kind 1)
- The same 24-bin chart, built from `SP_BATTERY` journal records. The metric row toggles LEVEL % (fixed 0–100) and VOLTAGE mV (`:96-102`, `:244`).
- The header shows live rail mV and the power source (ON BATTERY / EXTERNAL POWER / POWER UNKNOWN) from `hal_power_get` (`:149-151`).
- A strip under each bin is red for external power, white for active mode and grey otherwise (`:108-111`).
- "MODE LOG" opens SENSOR LOGS (`:247`). "IDLE 80MHz ON/OFF" toggles `nos_prefs.sensor_idle` and calls `prefs_save` (`:155`, `:247`).

### SENSOR LOGS (kind 2; `draw_logs` `fit_energy.c:158-184`, `input` `:251-256`)
- EVENT LOGS shows one record at a time, newest first, with NEWER / OLDER paging on `E.log_offset` and day navigation (`:182`, `:255-256`).
- RAW SENSORS mode is entered by tapping the title band (y 40–84, `:253`). It reads the hourly sensor files.
- Tapping the body (y 125–329) switches between fields 0–3 and 4–7 (`:254`).
- The header shows "SD WRITING / UNAVAILABLE", LOST and ERR counts from `sports_get_status` (`:160-162`).
- REPORT opens QUICK REPORT (`:256`).

### QUICK REPORT (kind 3; `draw_report` `:185-191`, `input` `:257-261`)
- Cycles the feature (10 categories, `:30`), the outcome (MISSED / FALSE TRIGGER / WORKED, `:31`) and the look-back window (30 / 60 / 300 s) (`:258-260`).
- SAVE LABEL writes a labelled REPORT record (`:261`).

### WRIST TUNING (kind 4; `draw_tune` `:192-209`, `input` `:262-279`)
- Feature cycles 1–9 (`:265`). Only features 1 (WALKING) and 2 (KNOCK LOCK) show a threshold and MORE / LESS SENSITIVE (`:196-202`, `:266-267`). The second parameter (entry peaks or quiet ms) is toggled by tapping y 140–185 (`:264`).
- Features 3–5 (RAISE WAKE, PICKUP, TURN WRIST) show "LEARN VIEW + START TRIAL" (`:204`). Starting calls `wake_service_learn()` (`:273`). The tick maps the learn state to a message (`:76-79`).
- START / END LABELLED TRIAL emits an SP_LABEL record (`:269`). Ending it pushes QUICK REPORT with the trial's feature and duration (`:270-274`, `:58`).
- RESET restores the default thresholds (`:277`, `tuning.c:28`). The "TRIAL: N REPS" selector (10 / 20 / 50, `:263`) is stored only as a label (`tuning.c:47`). Nothing counts reps.
- The footer shows "ACTIVE CONFIG REV n" from `tuning_revision()` (`:208`).

## Reads

Service and HAL getters, prefs and RAM state that the screens read on each tick or draw:

- **Sports journal query (async):** `sports_query_request` and `sports_query_poll` (`fit_energy.c:37`, `:67`). The worker reads the SD events file for the day and takes a snapshot of the NVS food ledger (`nos/sports.c:432-520`).
- **Event browser:** `sports_archived` and `sports_raw` read the SD day file or hourly sensor files. `sports_recent` falls back to the RAM ring of 96 non-raw records, and only for today (`fit_energy.c:72-73`; `sports.c:207-212`, `:559-589`).
- **Journal status:** `sports_get_status` (`fit_energy.c:160`; `sports.c:213-214`). It reports `available`, `dropped` and `errors`.
- **Labels:** `sports_name` and `sports_field` (`fit_energy.c:165`, `:169`; `sports.c:158-191`).
- **Day keys:** `sports_day(offset)` reads `hal_get_datetime` (`fit_energy.c:34`, `:255`; `sports.c:102-109`; `core/hal.h:29`).
- **Activity:** `svc_activity()` for today's `kcal` (`fit_energy.c:144`; `svc.c:438-450`). That is fed by `hal_activity_get` (`firmware/main/board.c:495-512`).
- **Power:** `hal_power_get` for `rail_mv` and `external_power` (`fit_energy.c:149`). The value is refreshed at 1 Hz by the battery sampler (`hal_esp32.c:316-388`; `board.c:1021`).
- **Wake learn:** `wake_service_get` for `learning` and `learn_result` (`fit_energy.c:77`; `core/wake_service.c:87-88`).
- **Tuning:** `tuning_get` (`fit_energy.c:195`, `:197-199`) and `tuning_revision` (`:208`). The detectors read the same values per sample (`firmware/main/board.c:270`, `:991`; `core/knock.c:29`; `core/walk.c:32`).
- **Prefs:** `nos_prefs.sensor_idle` (`fit_energy.c:155`, `:247`).
- **Engine and time:** `hal_millis`, `nos_revealed` (`fit_energy.c:40`, `:47`, `:57`, `:61`).
- **Not read:** the battery history ring (`firmware/main/hal_esp32.c:1226-1228`, dumped only over BLE), the Jobs history (`nos/background.c`), and the pedometer NVS key directly.

## Writes

- **Food and water (`sports_intake`, `sports_undo`):** write the NVS blob `sportsfood` (version, serial and 128 × 64-byte records, about 8.2 KB) and a FOOD or WATER journal record (`fit_energy.c:238`, `:249`; `nos/sports.c:338-365`). Undo writes a negative correction record in the same ring (`sports.c:355-359`).
- **Labelled reports (`sports_report`):** called on every open of QUICK REPORT, unlabelled (outcome −1), and again on SAVE LABEL (`fit_energy.c:54-60`, `:261`). Each call writes the NVS blob `sportsmarks` (16 slots, about 1 KB) and a REPORT record (`sports.c:196-206`).
- **Tuning (`tuning_adjust`):** writes the NVS blob `sportstune` and an SP_TUNE record (`fit_energy.c:267`, `:277`; `nos/tuning.c:25-44`). A zero direction restores the defaults (`tuning.c:28`).
- **Trial markers (`tuning_session`):** an SP_LABEL journal record only, with no storage write (`fit_energy.c:269`; `tuning.c:45-48`).
- **Wake learn (`wake_service_learn`):** sets RAM learning state only (`core/wake_service.c:80-86`). A successful learn saves NVS `wake_cfg` through `save()` (`wake_service.c:116-119`, `:60-67`).
- **Sensor-idle toggle:** sets `nos_prefs.sensor_idle` in RAM and calls `prefs_save()`, which is a debounced SD checkpoint (`fit_energy.c:247`; `nos/prefs.c:260-265`; `nos/prefs.h:55`).
- **Screen state:** `E.*` and the trial statics (`fit_energy.c:28-29`) are RAM only and are lost at reboot.

## Background behaviour

Producers and writers run whether or not Energy is open. Only the Energy tick is conditional.

- **`board-sense`** (priority 2, 4 KB static stack, `tskNO_AFFINITY`, `firmware/main/board.c:1036`). A 20 ms loop (`board.c:967-1034`) takes three IMU reads per cycle (`board.c:985-986`, `:1023-1025`). Each read feeds `sports_imu` (`board.c:300`), which writes to the SD raw journal, and the background observer (`nos/background.c:90-116`). The pedometer and `SP_WALK` run here too (`board.c:989-996`). The battery sample runs at 1 Hz (`board.c:1021`; `hal_esp32.c:316-376`) and emits `SP_RAIL`, plus `SP_BATTERY` on change (`hal_esp32.c:370-373`).
- **`sports_sd`** writer (priority 1, 4 KB stack in PSRAM, `tskNO_AFFINITY`, `nos/sports.c:282-293`, `:323`). It drains every ≥ 1 s in batches of up to 512, closes and fsyncs each batch (`sports.c:243-280`). It also runs `query_step` and wakes every 20 ms while a query is pending, otherwise every 1000 ms (`sports.c:290-291`).
- **`sports_tick`** runs at 1 Hz from `svc_tick` (`nos/svc.c:1319`; `sports.c:366-392`). It emits the minute SP_BATTERY and SP_ACTIVITY records, updates the clock and replays retained NVS records when the SD becomes available (`sports.c:379-382`).
- **`background_tick`** runs at 1 Hz from `services_tick` (`nos/apps/registry.c:62-66`; `nos/background.c:160-186`).
- **Screen off (PWR_OFF):** the loop runs at 50 Hz, but `svc_tick` and `background_tick` run only once per second (`firmware/main/nos_main.c:2091-2100`, `:2094`). The journal's 1 Hz tick and the minute records therefore keep running. The Energy tick does not run, because that branch calls only `nos_bg_tick`. `wake_service_tick` runs every loop (`nos_main.c:2038`).
- **AOD / SENSOR idle:** `nos_frame_run` runs at 1 Hz (`nos_main.c:2106-2121`). Screens tick only when they are on top, so Energy does not. The CPU is set to 80 MHz (`nos_main.c:2108`).
- **Energy tick:** runs only for the top screen and any screen in a transition (`nos/nos.c:2101-2109`). It does the 10 s history refresh (`fit_energy.c:70`) and the 1 s logs poll (`:71-75`). Both stop when the screen is covered or closed.
- **Pending queries** complete in the writer task after the screen closes. Their results sit in `sports_storage.query_result` until the next poll (`sports.c:454-464`).
- **Labelled trials** stay open after the screen closes. The Energy screen's `leave` callback is 0 (`fit_energy.c:283-287`), so leaving does not write an end record. Only the on-screen button does, and there is no timeout. The `trial_started` flag is RAM only (`fit_energy.c:29`).
- **Sleep gating:** board sleep fails while the journal queue is not empty or the journal is not quiesced (`firmware/main/board.c:1153-1155`). `sports_flush` runs in the stop path (`board.c:936`).
- **WRIST TUNING** has `SCR_KEEPAWAKE` (`fit_energy.c:287`). While it is on top, the display does not auto-sleep (`nos_main.c:2078-2086`).

## Persistence

- **SD (B only).** The card is one-bit SDMMC, CLK 41, CMD 39, D0 40 (`firmware/main/pins.h:57-59`, `BOARD_HAS_SD=1` at `:63`).
  - `/sdcard/watch/sports/YYYYMMDD-events.spt`: non-raw records (BOOT, POWER, BATTERY, ACTIVITY, WALK, KNOCK, WAKE, GESTURE, REPORT, LABEL, TUNE, FOOD, WATER, ROUTINE, LOCK).
  - `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt`: raw records, one file per local hour (IMU, ENV, LIGHT, MAG, HEART, GPS, PPG, RAIL, LIGHT_RAW, MAG_RAW, MIC, TOUCH).
  - Records are 64 bytes with an FNV-1a checksum and magic `0x53505431` (`nos/sports.h:7`, `:13-18`; `sports.c:87-93`). Files are appended, and a partial tail record is recovered on open (`sports.c:223-232`). Nothing prunes these files (no `remove`, `unlink` or `rename` in `sports.c`).
- **NVS (non-prefs keys on B).** `hal_store_save_checked` sends every key except `nosprefs` and `wifi_cred` to NVS (`firmware/main/hal_esp32.c:228-236`).
  - `sportsfood`: the food/water ring (128 entries, about 8.2 KB, `sports.c:70`, `:310-311`, `:347`, `:361`).
  - `sportsmarks`: the report ring (16 entries, about 1 KB, `sports.c:71`, `:202`, `:313-314`).
  - `sportstune`: the walk and knock tuning blob (`tuning.c:18`, `:40`).
  - `wake_cfg`: wake configuration, written on learn success (`core/wake_service.c:63`).
  - `pedometer` and `nosact` feed today's kcal. They are written elsewhere (`firmware/main/board.c:915`; `nos/svc.c:499`).
- **SD prefs.** `sensor_idle` lives in the `nosprefs` checkpoint, written under `/sdcard/watch` (`hal_esp32.c:236-253`; `nos/prefs.h:33`).
- **RAM only.** The five `energy_state` structs (about 1.1 KB each, `fit_energy.c:15-23`), the trial statics (`:28-29`), the query job and result (`sports.c:62-67`, `:74-75`), the 96-entry `recent[]` ring (`sports.c:69`, `:84`), and status counters.
- **Persistence reliability.** NVS ring writes are checked (`sports.c:202`, `:347`, `:361`), and a failed save is rolled back (`sports.c:203`, `:348`, `:362`). Failures are shown as "SAVE FAILED" or "MARK SAVE FAILED" (`fit_energy.c:239`, `:59`).

## Navigation

- **Opened from the launcher.** Energy, Battery history, Sensor logs, Wrist tuning and Quick report are all launcher apps (`nos/apps/registry.c:11`, `:14-17`).
- **Opened from Control centre.** The Quick Tools drawer pushes Quick report, Energy, Battery history and Wrist tuning with `TX_PUSH` (`nos/apps/sys_control.c:494-495`, `:657`).
- **Opened from Jobs.** The job detail maps CALORIES to Energy, BATTERY to Battery history and every other job without its own screen to SENSOR LOGS (`nos/apps/sys_background.c:213-220`).
- **Internal links.** Battery → Sensor logs (`MODE LOG`, `fit_energy.c:247`). Sensor logs → Quick report (`:256`). Wrist tuning → Quick report on END TRIAL (`:274`) and → Sensor logs on LOGS (`:278`). Energy → its own in-screen editor (`:126`, `:248`).
- **Back.** The five screens have no back button. They rely on the engine's back gesture. A revealed screen keeps its state (`fit_energy.c:47`, `:57`, `:61`).

## Gaps

Ordered by severity. Each entry has the code evidence and the user-visible effect. Where a defect depends on a condition, the condition is stated.

1. **[high] Today's food and water totals show zero when the SD path fails, even though the NVS ledger holds them.** `refresh()` zeroes the bins and sets "HISTORY UNAVAILABLE" (`fit_energy.c:36-38`). NVS entries go only into the query job (`sports.c:438-447`). A failed query returns −1 and never copies bins (`sports.c:454-463`). Failure comes after 3 s without the SD lock, or on a read error (`sports.c:476-480`, `:516`). Effect: the screen says "ENTRY SAVED" and then shows 0 EAT / 0 mL with an error, so the logged food is not visible.
2. **[high] Food and water logging is refused once the 128-entry ring wraps, unless the SD is mounted and the old slot is already exported.** `sports_intake` calls `archived_entry` on the slot it is about to overwrite (`sports.c:341-342`). That returns 0 when the SD lock is unavailable, or when the record is not yet in the day file (`sports.c:326-337`). The check is a linear scan of the day file on the UI task. The ring counts every intake and undo, so this begins after 128 lifetime writes (`sports.c:45`). Effect: "SAVE FAILED - TRY AGAIN" with no card (`fit_energy.c:239`).
3. **[high] The raw IMU journal is always on and never pruned.** Every IMU read calls `sports_imu` (`board.c:300`), with no recording or mode gate. Three reads run per 20 ms cycle (`board.c:985-986`, `:1023-1025`), roughly 120–150 records per second at 64 bytes each. That is about 9 KB/s, or about 32 MB per hour of SD writes, while the screen is off. Nothing prunes the files (`sports.c`, no `remove`, `unlink` or `rename`). This agrees with [[Storage and persistence]] and the Calorie trace note (`docs/os-map/systems/Calorie trace.md:21`, `:137-141`). Effect: the card fills, and the RAW SENSORS view has an unbounded source.
4. **[medium] Retained food and report records are replayed into the journal on every boot.** `sports_init` enqueues them (`sports.c:320-321`). The first SD-available transition enqueues them again (`sports.c:379-382`). `was_available` starts at 0 each boot (`sports.c:372`). Effect: duplicate rows in SENSOR LOGS and the day file. Totals are deduplicated by `seq`, but the event browser is not. This is from source reading; the duplicates have not been counted on a card.
5. **[medium] Opening QUICK REPORT writes a false event, and SAVE LABEL defaults to MISSED.** `report_enter` writes an unlabelled REPORT on every open (`fit_energy.c:54-60`, `sports.c:196-206`), including when the user backs out. The outcome defaults to 0, which is MISSED, and SAVE LABEL saves it unchanged (`fit_energy.c:48`, `:188`, `:261`). Effect: logs fill with unlabelled marks and the default labels are presented as user judgements.
6. **[medium] The full day's journal is re-read every 10 s while Energy or Battery history is on top.** `tick` calls `refresh()` every 10 s (`fit_energy.c:70`). Each refresh resets the job (`sports.c:443`) and re-reads from byte 0 (`sports.c:471`, `:484-504`). Estimate: a full day has about 2 minute records per minute (`sports.c:384-391`), so roughly 180–200 KB per refresh, before event records. Effect: sustained SD reads and writer time while the screen is open. This is a power cost, see [[Power and battery]].
7. **[medium] Drag-zoom has no minimum width, so a repeated zoom produces an empty range that reads as an error.** Zoom computes `start` and `end` by integer division (`fit_energy.c:224-226`). A two-bin zoom from the full day gives 120 min, then 10 min. Another zoom makes `start == end`, which `sports_query_request` rejects (`sports.c:434`). The screen then shows "HISTORY UNAVAILABLE" (`fit_energy.c:38`) even with the SD working.
8. **[medium] The burn chart is a cumulative staircase, and a zoomed range shows the cumulative value, not the energy in that range.** Each bin keeps the latest cumulative kcal (`sports.c:409-411`). The summary takes the maximum over the zoomed bins (`fit_energy.c:143`) and labels it "BURNED: DAILY ESTIMATE" (`:147`). Effect: a zoomed window shows the day's running total, not that window's energy.
9. **[medium] The energy figure is steps × 0.04 kcal only.** There is no resting energy, no heart-rate term and no body mass (`svc.c:45-46,445`). `sports_mass_kg` affects only workouts (`fit_workout.c:110`, `:633`). The footer admits "ACTIVITY CALORIES ONLY" (`fit_energy.c:156`). Effect: the number is far below a modern watch's total daily energy.
10. **[medium] Five of the ten wrist-tuning features have no adjustable detector, and the screen does not say so.** Features 0 (GENERAL), 6 (AIR GESTURE), 7 (TOUCH), 8 (GPS) and 9 (RECORDER) get only a label trial (`fit_energy.c:262-279`; `tuning.c:45-48`). For features 3–9 the screen still shows "LEARN VIEW: HOLD WATCH TO READ" (`fit_energy.c:203`), even where there is no learn view. Effect: the labels are presented as tuning actions when they are not.
11. **[medium] A pickup or turn trial does not check whether that detector is enabled.** `configure()` enables pickup and turn from the saved flags (`core/wake_service.c:42-50`). The trial starts without reading or showing those flags (`fit_energy.c:269-273`). Effect: a trial can be run with the detector off and produce no useful result.
12. **[medium] Trial state is RAM only, has no time limit, and reps are never counted.** `trial_started`, `trial_feature` and `trial_expected` are statics (`fit_energy.c:28-29`). They are lost at reboot, and nothing ends a trial automatically. "TRIAL: N REPS" is recorded as a label (`tuning.c:47`) and never compared with a count.
13. **[low] UNDO is single-step, does not check type, and gives one message for two outcomes.** A second UNDO is refused because the latest record is now a negative correction (`sports.c:355-356`). The message "NOTHING TO UNDO / SAVE FAILED" covers both "nothing to undo" and a failed save (`fit_energy.c:249`). There is no per-entry edit or delete.
14. **[low] The Control centre tool "LOGS / WRIST TUNING" opens only Wrist tuning.** `tool_screens[3]` is `scr_wrist_tuning` (`nos/apps/sys_control.c:494-495`). Sensor logs are reachable only from Battery, from Wrist tuning, or from Jobs.
15. **[low] "IDLE 80MHz ON/OFF" is misleading.** The screen-off branch already sets 80 MHz (`nos_main.c:2092`). The toggle selects only between the SENSOR state (1 Hz display) and OFF (`nos/prefs.h:33`; `nos/power_policy.h:13-14`). The default is ON (`nos/prefs.c:159`, `:171`), and the change takes effect at the next idle transition (`nos_main.c:2085`).
16. **[medium] No phone sync for food, water or the journal.** Nothing in `nos/gb.c` or the BLE code references food, water or the sports journal. The only export is the desktop tool `tools/sports_export.py`, which reads an SD card on a host (`tools/sports_export.py:1-5`). See [[Connectivity and voice]].
17. **[low] Battery history has no level data while charging, no time-to-empty and no drain rate.** Level samples count only when `battery_valid` (`nos/sports.c:412-414`). That is 0 on USB (`firmware/main/hal_esp32.c:336-337`). A separate 24-hour RAM ring (`hal_esp32.c:1226-1228`, `:1270-1282`) is dumped only over BLE and is not shown on the watch.
18. **[low] Sensor logs browse one record at a time, with no jump, no search and no end-of-log bound.** OLDER increments without a limit (`fit_energy.c:256`). The RAW toggle is a hidden tap on the title band (`:253`), and field paging is a hidden tap in the body (`:254`).
19. **[low] Without the SD, Sensor logs show only RAM events for today.** The fallback is `sports_recent` (`fit_energy.c:73`; `sports.c:207-212`), which indexes a different list from the file (`fit_energy.c:72-73`). Mixed states can repeat or skip rows.
20. **[low] Records logged before a trusted clock are stamped with day 0 and never match a day query.** `clock_day` is 0 without an epoch (`sports.c:376`), and a record's `day` comes from it (`sports.c:115`). Queries match on `day` (`sports.c:446`). Effect: food logged with no trusted clock disappears from totals, while NVS still holds it.
21. **[low] NVS writes run synchronously on the UI task.** Food and water save and undo write the 8.2 KB `sportsfood` blob (`fit_energy.c:238`, `:249`; `sports.c:347`, `:361`). Opening QUICK REPORT writes `sportsmarks` (`fit_energy.c:59`; `sports.c:202`). Not measured, so the stall length is unknown.
22. **[low] Transition ghosting may occur and is unverified.** All five screens share one `draw` and read the global `active` state (`fit_energy.c:26`, `:210-214`). A covered screen is drawn through its own callback during transitions (`nos/nos.c:1126-1129`, `:1179-1193`). A per-screen layer cache may hide this. It needs a frame capture on the bench.
23. **[low] "ACTIVE CONFIG REV" is a per-boot counter, not a persistent identity.** `tuning_init` does not restore the revision (`nos/tuning.c:13-14`, `:41`). The label does not say so.
24. **[low] The Jobs routing sends HEART, GPS, CLIMATE, LIGHT, COMPASS and JOURNAL to the unfiltered event browser.** These fall into the default branch of `records_screen` (`nos/apps/sys_background.c:213-220`).
25. **[low] Sensor logs reads the SD every second while open.** `tick` re-reads the newest record each second (`fit_energy.c:71-75`). In RAW mode, each call may open up to 24 hourly files (`nos/sports.c:563-564`).

## Docs check

- `docs/os-map/systems/Health and activity.md:127` says "With no SD it shows HISTORY UNAVAILABLE". That is the message, but gap 1 also zeroes the totals.
- `docs/os-map/systems/Calorie trace.md:129`, `:347` (the cumulative staircase) matches `sports.c:409-411`.
- `docs/os-map/systems/Calorie trace.md:137-141` (raw IMU always on, no pruning, 512 per drain) matches `board.c:300` and `sports.c:46`. The 150 Hz figure is consistent with three reads per 20 ms cycle.
- `docs/os-map/systems/Calorie trace.md:257` (writer task, 4 KB, PSRAM, priority 1, `tskNO_AFFINITY`) matches `sports.c:323`. The task name says "Pinned" but the affinity is none.
- `docs/os-map/systems/Engine and scheduling.md:70` (board-sense static, unpinned, priority 2, 4 KB) matches `board.c:1036`.

## Evidence limits

Nothing was built, flashed or run. The serial port and the watch were not used. Behaviour is from source reading. Gap 4 (replay duplicates), gap 22 (ghosting) and the power estimates in gaps 3 and 6 need a card or bench measurement to confirm.
