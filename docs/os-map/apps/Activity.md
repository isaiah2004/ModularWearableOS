---
title: "Activity"
id: app-activity
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Activity

Source: `nos/apps/fit_activity.c` (589 lines). Read-only source review; nothing built, flashed or run on a board. Evidence is `repo-relative/path:line`.

## Summary

Activity is the NOS day-steps screen: a three-page vertical pager (`nos/apps/fit_activity.c:3-7`). Page 0 is the steps hero, page 1 is today (distance, kcal, active minutes, 24-hour chart), page 2 is the last 7 days. It is also Home widget slot 2 (`nos/apps/sys_home.c:203`, `fit_activity.c:583`).

The screen owns no sensor, no storage and no settings. Each tick it copies `svc_activity()` (`fit_activity.c:44-53`) and draws it. Step counting happens elsewhere: the 1.43-B IMU feeds a software pedometer (`core/walk.c`, `core/pedometer.c`) inside the `board-sense` task, and the UI-task service `svc_activity` turns that total into day data. See [[Health and activity]], [[Hardware and drivers]] and [[Services hub]].

Hardware it depends on: the QMI8658 IMU at I2C 0x6B, interrupt pin 8, on the 1.43-B (`firmware/main/pins.h:53,61`). A trusted clock and time zone are also needed (`firmware/main/board.c:158`, `firmware/main/nos_main.c:963-965`). The screen itself needs only the display. The C board has no IMU (`firmware/main/pins.h:27`).

Status is "working" from source reading. It was not confirmed on hardware in this pass.

## Features

- **Steps hero (page 0).** Dot-matrix count inside a 48-segment glyph ring toward the goal, with a red lap past the goal. Labels "STEPS" and "N% OF <goal>", or "GOAL <goal>" at zero (`fit_activity.c:196-220`, `fit_activity.c:76-84`). The number builds in 0.30 s (`nos/ui.h:68`). The ring sweep is 0.5 s plus 0.85 s per lap (`fit_activity.c:150-156`). Live changes morph at most once per second (`fit_activity.c:34`, `fit_activity.c:190-192`).
- **Count format.** A thousands separator appears only from 10,000 (`fit_activity.c:57-63`).
- **Today (page 1).** Distance in km or mi, kcal, and "ACTIVE" minutes (`fit_activity.c:403-410`). Also a 24-column dot chart of steps per hour: grey for past hours, white for the current hour, grid only for future hours, and a red NOW dot (`fit_activity.c:462-487`). Axis labels are 00, 06, 12 and 18.
- **This week (page 2).** "AVG / INCLUDES TODAY", averaged over observed days only (`fit_activity.c:421-430`). Seven day bars with weekday letters, today in white, and a GOAL line (`fit_activity.c:490-517`).
- **Pager.** Vertical spring pager with rubber-band ends, velocity-based page choice, and a haptic tick on each page change (`fit_activity.c:223-285`). The scrollbar flashes for 0.7 s on entry (`fit_activity.c:286-287`, `fit_activity.c:341`).
- **Units.** Km or mi follows the Settings Units toggle live (`fit_activity.c:402`, `fit_activity.c:448`).
- **Home widget.** The same hero at a smaller ring with no red. Tapping it opens the full app with the iris transition (`fit_activity.c:549-583`).
- **Revealed by back.** Returning from a cover keeps the built page and hero (`fit_activity.c:332-336`).

## Reads

Service getters:
- `svc_activity()` (`nos/svc.h:102`, `nos/svc.c:438`). Read in `read_data()` (`fit_activity.c:44-53`), which runs every tick of the app and of the Home widget (`fit_activity.c:567`). Fields used: `steps`, `goal`, `km`, `kcal`, `active_min`, `hourly[24]`, `week[0..6]`, `week_valid`.
- Not read by this screen: `stand_hours` (`nos/svc.h:97`), `checkpoint_pending`, `checkpoint_errors`, `checkpoint_epoch` (`nos/svc.h:98-100`).
- No other service getter is called. `hal_activity_get()` is not called here. Only `svc.c` and `background.c` call it.

Prefs:
- `nos_prefs.step_goal_100`, fallback when the goal is 0 (`fit_activity.c:48`). Default 80, meaning 8000 (`nos/prefs.c:155`).
- `nos_prefs.units_metric` (`fit_activity.c:402`, `fit_activity.c:448`).

HAL:
- `hal_get_datetime()` for local hour and weekday (`fit_activity.c:49-51`, `firmware/main/hal_esp32.c:142-146`).

Engine and frame state:
- `nos_entered_by_finger()`, `nos_revealed()`, `nos_enter_t()`, `nos_t`, `nos_frame` (`fit_activity.c:332-342`, `fit_activity.c:448`, `nos/nos.h:152,178,189`).
- `pblur_baking` for the widget bake (`fit_activity.c:577`).

## Writes

- **RAM only.** The `s_act` copy, `s_hour` and `s_wday` (`fit_activity.c:39-40`). Pager, hero and page-build state in `A` (`fit_activity.c:304-312`). The `prepared` cache and its frame stamps (`fit_activity.c:387-395`). Widget state `W` (`fit_activity.c:552-555`). The motion-blur track (`fit_activity.c:536`).
- **Haptic and sound.** `nos_feedback(FB_TICK)` on page change (`fit_activity.c:266`) and once when the entry ring completes (`fit_activity.c:182`). See `nos/nos.c:557-569`.
- **Opening the app from the widget.** `nos_open(&scr_activity)` (`fit_activity.c:581`) sets the last-app pointer, iris origin, and FB_OPEN feedback (`nos/apps/registry.c:45-53`).
- **Sim-only probes.** `nos_motion_probe` (`fit_activity.c:362-379`). It is a no-op on the device (`nos/nos.h:330-332`).
- **Nothing persisted.** No prefs writes, no svc mutators, no NVS or SD writes, no phone messages. The only input handler is pager drag (`fit_activity.c:542-545`).

## Background behaviour

The screen itself does no background work. `act_tick` runs only while it is the top screen. The data pipeline runs regardless of the screen:

1. **`board-sense` task.** Priority 2, 4 KB stack allocated from PSRAM, no core affinity (`firmware/main/board.c:1028-1047`). Loop period about 20 ms (8 + 8 + 4 ms delays, `board.c:1023-1025`). Each cycle reads the IMU, calls `pedometer_update()` (`board.c:989`), and publishes `s_activity` (`board.c:1009`). It parks only in the sleep handshake for deep sleep or power-off (`board.c:1126-1150`, `firmware/main/main.c:521,535`, `firmware/main/nos_main.c:797`).
2. **`board-fs` task.** Priority 1, 100 ms loop (`board.c:919-966`). Saves NVS key `pedometer` every 60 s on success or 5 s on failure (`board.c:954-955`). Save is gated by `s_background_persistence`. That flag is set in the PWR_OFF, AOD and SENSOR branches (`nos_main.c:2098,2110`) and forced to 0 while the display is ON (`nos_main.c:2141`). The task also flushes on the shutdown handshake (`board.c:930`).
3. **`svc_tick` services.** Called once per frame of the display-ON render loop (`nos/nos.c:2299-2341`, from `firmware/main/nos_main.c:2155`). Called at 1 Hz with the display OFF or in AOD (`firmware/main/nos_main.c:2094`). The slow path runs `act_tick()` and `sports_tick()` once per 1000 ms (`nos/svc.c:1315-1320`). `act_tick()` updates hourly bins, active minutes and the week record, and saves NVS `nosact` (`nos/svc.c:529-561`).
4. **Background jobs.** `background_tick()` runs at most once per second (`nos/background.c:160-164`). It publishes "Step tracking" and "Calorie estimate" for the Jobs screen (`nos/background.c:22-23`, `nos/background.c:127-133`).
5. **Gadgetbridge realtime.** If the phone enables it with `{"t":"act"}`, svc sends step deltas every `int` seconds while the phone is connected (`nos/svc.c:1140-1146`, `nos/svc.c:1180-1188`, `nos/svc.c:1243-1246`).
6. **Sports journal.** `sports_tick()` copies steps, kcal and active minutes into the journal about once a minute (`nos/sports.c:384-392`).

Screen off: steps, hourly bins and active minutes keep counting at 1 Hz. The Activity screen is not drawn.

## Persistence

All keys live in NVS namespace `wos` on the 1.43-B, because `hal_store_save_checked()` routes every key except `nosprefs` and `wifi_cred` to NVS (`firmware/main/hal_esp32.c:236`, `firmware/main/hal_esp32.c:177-186`, `firmware/main/hal_esp32.c:258-285`).

| Key | Contents | Written | Read |
|---|---|---|---|
| `pedometer` | `pedometer_saved` v1: version, day, steps, reserved, active_ms (`core/pedometer.h:4-8`) | `board.c:903-917` (NVS key at `board.c:915`). 60 s cadence, display OFF/AOD only, plus shutdown flush | Boot, `board.c:1069` |
| `nosact` | `act_saved` v2: valid_days, day, six previous days, hourly[24], active_min, last_steps, max_steps (`nos/svc.c:427-435`) | `svc.c:496-509`. Every 10 min when dirty, 5 s retry on failure (`svc.c:558`). Also on day rollover (`svc.c:527`). Not written at shutdown | `svc.c:562-577`. Accepts version 1 and migrates it |
| `nosprefs` | Step goal and units. This screen only reads it | Settings (`nos/apps/set_settings.c:1936`) | SD `/sdcard/watch/settings.{0,1}.bin` with NVS mirror (`firmware/main/hal_esp32.c:238-256`, `nos/prefs_disk.c:32`) |

Other facts:
- The simulator uses a synthetic day and no persistence (`nos/svc.c:458-479`).
- The screen persists nothing of its own.
- Maximum-loss windows: up to 10 min for hourly and week data (`svc.c:558`). While the display is ON, the raw count is not written at all (`board.c:954-955`, `nos_main.c:2141`).

## Navigation

Opens from:
- Launcher: first entry, "Activity" (`nos/apps/registry.c:9`).
- Home widget page 2: tap (`nos/apps/sys_home.c:203`, `sys_home.c:1128-1129`, `fit_activity.c:581`).
- Face hit areas: the inverted face (`nos/faces/face_inverted.c:165-171`), the segment face (`nos/faces/face_segment.c:612,616`), and a row on the split face (`nos/faces/face_split.c:225`).
- Jobs screen: "Step tracking" record button (`nos/apps/sys_background.c:214`, `sys_background.c:245`).
- Last-app double press (`nos/apps/registry.c:40-43`).

Opens: nothing. The screen has no buttons and no tap handler (`fit_activity.c:542-545`).

Settings: "Step goal" and "Units" sit under an "Activity" header (`nos/apps/set_settings.c:477-481`). The goal is picked in 500-step steps, from 2000 to 30000 (`set_settings.c:1927-1936`, clamp at `set_settings.c:1952`). The "Wrist" row under the same header feeds Sports placement, not this screen (`nos/apps/fit_workout.c:105`).

## Gaps

- **Steps can drop after an unplanned reset while the display is ON (medium).** Evidence: `board.c:954-955` and `nos_main.c:2141` mean the raw count is not saved while the display is on. `svc.c:558` saves `nosact` every 10 min, and that record already holds the higher hourly bins. After a reset the restored pedometer count can be lower than the binned total. `svc.c:541` then lowers `last_steps` to match, so the hero and day total stay low while the hourly chart keeps the higher count. The two views disagree until midnight.
- **Hourly and active-minute history is not flushed on shutdown (low).** Evidence: `svc.c:558` is the only save path, with no shutdown call. The raw count is flushed at shutdown (`board.c:930`), so after a normal power-off up to 10 min of hourly and active-minute data is missing. The missing delta is credited to the boot hour (`svc.c:541-548`). Week totals are saved at rollover (`svc.c:527`) and are not affected.
- **No validity signal (medium).** `svc_activity_t` has no valid flag (`nos/svc.h:91-101`). When `hal_activity_get()` is invalid, svc publishes the persisted `max_steps` as today's count (`svc.c:533`). This happens while prefs load, while the time zone is not ready, when the clock is untrusted, or when the sample is older than 250 ms (`firmware/main/board.c:495-506`). The screen draws the value as live, with no waiting or stale state. A time-zone change also clears the calendar gate (`board.c:832`).
- **Unobserved days look like zero days (low).** The average drops unobserved days (`fit_activity.c:421-427`). Drawing does not: all seven bars are drawn without a validity mark (`fit_activity.c:510-516`). Gap days after a long absence are marked unknown (`svc.c:514-519`).
- **Distance and kcal are fixed estimates (medium).** Stride is 0.75 m (`svc.c:45`, `svc.c:444`) and energy is 0.04 kcal per step (`svc.c:46`, `svc.c:445`). There is no height, stride or sex field in `nos/prefs.h`. The only body field is `sports_mass_kg` (`nos/prefs.h:37`), which this screen does not read. Kcal is labelled "KCAL" with no caveat.
- **"ACTIVE" is a cadence heuristic, not active time (medium).** A wall-clock minute counts if it has more than 60 steps (`svc.c:550-556`). The pedometer's `active_seconds` (`core/pedometer.c:25-27`, `core/hal.h:283-286`) has no NOS reader. A 61-step burst counts as an active minute, and slow walking (under about 40 steps per minute) never does.
- **Stand hours are computed but never shown (medium).** `stand_hours` is set in `svc.c:451-453` and has no reader outside svc (`nos/svc.h:97`). `move_goal_10` is reserved and unused (`nos/prefs.h:27`). There is no stand or active-minute goal and no second ring.
- **Seven days only, no drill-down (medium).** The week array holds six previous days plus today (`nos/svc.h:96`). The hourly curve resets at rollover (`svc.c:520`). Bars and hourly columns cannot be tapped (`fit_activity.c:542-545`). There is no monthly view, trend or export.
- **Taps do nothing (low).** Only pager drag is handled (`fit_activity.c:235-241`, `fit_activity.c:542-545`). There is no link to Settings goal, Sports or Heart.
- **No goal-reached event (low).** The only goal logic is the colour and the "% OF" label (`fit_activity.c:76-84`, `fit_activity.c:216-219`). The one haptic is at ring entry (`fit_activity.c:182`). No event or notification exists in `svc.c`.
- **Walking state unused (low).** `hal_activity.walking` (`core/hal.h:283-286`) is read only by the Jobs screen (`nos/background.c:131`). There is no move or sedentary reminder.
- **Detection limits (low).** Steps count only inside walk bouts: at least 8 coherent peaks, with 300-1500 ms intervals (`core/walk.c:9-13`, `core/walk.c:59-64`). Cadence outside about 40-200 steps per minute is not counted. The detector is a generic cadence test with no activity-type classification, so non-walking motion can be miscounted or missed.
- **Label format mismatch (low).** The hero's "OF" and "GOAL" labels print the raw goal, for example 30000, while the number uses 30,000 (`fit_activity.c:217-218`, `fit_activity.c:58-63`).
- **No sleep, heart or workout context (low).** The screen does not link to Heart (`nos/apps/fit_heart.c`) or Sports (`nos/apps/fit_workout.c`). No sleep-tracking screen exists in `nos/apps/`.
- **No data path on boards without an IMU (low, not 1.43-B).** On the C board `BOARD_HAS_IMU` is 0 (`firmware/main/pins.h:27`), so `hal_activity` never becomes valid. The screen silently shows the persisted maximum (`svc.c:533`).
- **Misfiled setting (low).** "Wrist" sits under the Activity header (`set_settings.c:479`). It feeds Sports placement (`fit_workout.c:105`, `fit_workout.c:111-114`). It is not an input to the pedometer, whose `pedometer_update()` call takes no wrist argument (`board.c:989`).

## Related

- [[Health and activity]]: steps, the pedometer and the `svc_activity` pipeline.
- [[Sports and workouts]]: the Sports journal copy at `nos/sports.c:384-392`.
- [[Power and battery]]: screen-off timing and the 1 Hz loop.
- [[Storage and persistence]]: NVS `wos` keys, the SD settings checkpoint and the shutdown handshake.
- [[Hardware and drivers]]: IMU, RTC and pin map.
- [[Connectivity and voice]]: Gadgetbridge `act` realtime messages.
- [[Shell and faces]]: launcher, Home pager, face hit areas and the Jobs screen.
- [[Engine and scheduling]]: `nos_bg_tick`, screen lifecycle and frame timing.
- [[Services hub]]: where `svc_activity` sits among the services.
