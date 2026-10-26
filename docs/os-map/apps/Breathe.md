---
title: "Breathe"
id: app-breathe
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Breathe

## Summary

Guided paced breathing, one screen (`scr_breathe`, `nos/apps/fit_breathe.c`, 575 lines; declared at `nos/apps/nos_apps.h:44`). It is launcher tile "Breathe" with `DOT_ICON_BREATHE` (`nos/apps/registry.c:19`). It has no service, no task and no storage of its own. It only runs while it is the top screen.

- **Cycle**: fixed 4 s inhale, 2 s hold, 6 s exhale, which is 5 breaths per minute (`fit_breathe.c:28-31`, `:201`).
- **Sessions**: 1, 3 or 5 minutes (`fit_breathe.c:91`). The default selection is index 1, which is **3 MIN** (`fit_breathe.c:92`).
- **Cues**: the start disc breathes, a 12-capsule dial shows progress, and the phase word is shown. Haptics mark phase starts when `nos_prefs.haptics` is on (`fit_breathe.c:105`, `:331-336`). The screen has no audio.
- **Heart rate, optional**: if the MAX30102 strap answers its probe (`EXT_HR`), the session requests the strap stream under owner `HAL_HR_OWNER_BREATHE`. It integrates bpm over acquisition time. At a natural end it writes the average into the services hub's RAM heart state (`fit_breathe.c:158-191`, `:348-356`). On a bare 1.43-B there is no strap, and the summary reads "HEART RATE UNAVAILABLE".
- **Persistence**: none. Nothing is written to NVS, SD or prefs. The only lasting effect is the RAM heart state in `nos/svc.c` (see [[Services hub]] and [[Health and activity]]).
- **Hardware needed**: a touch display (B: FT3168, per the workspace CLAUDE.md table). The strap is optional: MAX30102 at I2C 0x57 for HR (`nos/hal_ext.h:60`, probe in `firmware/main/ext/ext_hr.c:310-322`), and a strap vibration motor on IO16 for haptics (`nos/hal_ext.h:1-12`, `firmware/main/ext/ext_priv.h:33`). It needs no IMU, RTC, SD or audio. A trusted clock is needed only to stamp the recorded average (`nos/svc.c:619-623`).
- **Status: partial.** The state machine, cues, summary and HR integration are all implemented, and the code is internally consistent on reading. This card is from source only, not a watch run. Main gaps: the idle timer collapses a finished summary to home; a stray swipe or side-button press silently discards a session; nothing is saved (see [[#Gaps]]).
- **Not this app**: `core/apps/app_breathe.c` is a separate legacy WOS "Breathe" app (`core/apps/app_breathe.c:243`). It is not in the NOS registry and is not covered here.

Hardware is covered in [[Hardware and drivers]]. Strap behaviour is in [[Health and activity]].

## Features

- **START screen**: "BREATHE" title, a white START disc (r 44 at 233,204; `fit_breathe.c:42-43`, `:448`, `:440-443`), and three pills **1 MIN / 3 MIN / 5 MIN** (`fit_breathe.c:453-455`). Tap the disc to start (`:558`). Tap a pill to choose (`:559-560`). Selection crossfades over 4 frames (`:287-290`).
- **Intro**: 1 s. START sinks into the breath, the pills scale out, and "Breathe in" fades in 0.2 s before the first inhale (`fit_breathe.c:296-323`).
- **Session**: the disc grows from r 44 to r 74 on inhale and shrinks on exhale, with a linear share in the easing (`:97-115`, `:247-257`). The phase word reads "Breathe in", "Hold" or "Breathe out" (`:459-465`, `:338-347`).
- **Progress dial**: 12 capsules around radius 105, lit clockwise by elapsed fraction in eighths of a capsule (`:46-49`, `:144-155`, `:256`).
- **Haptics** (only when `nos_prefs.haptics`): TICK at inhale start, DOUBLE at exhale start, DOUBLE at session end (`:331-336`, `:354`). **The hold has no cue.**
- **Outro**: 11 frames. The disc closes to a point and the dial fades (`:359-373`).
- **Summary**: "COMPLETE", the nominal minutes, "N BREATHS" (the scheduled cycle count, `:482`), "AVG N BPM" or "HEART RATE UNAVAILABLE", and "HR COVERAGE N%" (`:467-497`).
- **AGAIN** rebuilds the field and starts a new session with the same duration, skipping START (`:374-385`, `:561`). **DONE** pops the screen (`:562-566`).
- **Heart-rate integration** (only with the strap): valid samples need bpm 35-230, quality 0.65-1.0, on wrist, and age <= 3 s (`:173-175`). It uses trapezoidal integration over acquisition time with gaps <= 3 s (`:178-184`). The average needs >= 10 s valid and >= half the session (`:187-191`). The integration is covered by a host test (`tests/test_health_regressions.py:153-184`; not run for this card).
- **Controls**: all four controls are live (disc, three pills, AGAIN, DONE). No dead button was found. The tapped control shows pressed for >= 0.10 s (`:34`, `:552`).

## Reads

### Services

- `svc_heart_sample_epoch(sample_ms)` (`fit_breathe.c:185`, defined `nos/svc.c:619-623`). It converts monotonic age to epoch. It returns 0 when the clock is untrusted (before 2020-01-01) or when the sample is more than 3 s old.
- **No `svc_*` getter is read.** Breathe does not read `svc_heart()`, `svc_activity()`, `svc_sys()` or any other service data.

### Prefs

- `nos_prefs.haptics` (`fit_breathe.c:105`; default 1 at `nos/prefs.c:138`). `nos/nos.c:570` reads the same flag for `nos_feedback()`.
- No other prefs key is read. Nothing is read from NVS or SD.

### HAL

- `hal_ext_present()` with the `EXT_HR` bit (`fit_breathe.c:162`, `:213`; `nos/hal_ext.h:60`).
- `hal_hr_get(&hal_hr)`, fields `on_wrist`, `bpm`, `quality`, `sample_ms` (`fit_breathe.c:173`; `nos/hal_ext.h:202-215`). The cached read is in `firmware/main/ext/ext_hr.c:343-356`. Values older than 2 s are zeroed.
- `hal_millis()` for session timing (`fit_breathe.c:161`, `:172`, `:322`).
- `hal_haptic(pattern)` with `HAPTIC_TICK` and `HAPTIC_DOUBLE` (`fit_breathe.c:105`, `:334-335`, `:354`; `nos/hal_ext.h:72-80`).

### Engine (control, not data)

`nos_t`, `nos_enter_t()`, `nos_entered_by_finger()`, `nos_revealed()`, `nos_leaving_covered()`, `nos_keep_awake()`, `nos_feedback()`, `nos_pop()`. `nos_motion_probe()` is compiled out in firmware (`nos/nos.h:328-333`).

## Writes

### Hardware requests

- `hal_hr_request(HAL_HR_OWNER_BREATHE, 1)` at session start (`hr_start`, `fit_breathe.c:158-163`) and on a reveal during INTRO or SESSION (`:210-214`).
- `hal_hr_request(HAL_HR_OWNER_BREATHE, 0)` in `hr_stop` (`:164-168`). `breathe_leave` calls it unless the screen is covered (`:224-230`).
- Owner bit 4 is set or cleared in the strap lease (`firmware/main/ext/ext_hr.c:330-341`). Only Breathe's own bit changes.
- `hal_haptic()` as listed above.

### Services

- `svc_heart_record_at(B.hr_avg, 0, B.hr_epoch)` runs once, at a natural end, and only when the average is non-zero (`fit_breathe.c:348-356`). It sets `s_heart.bpm_last` and `bpm_last_epoch`. It updates the day min and max (`s_hr_rec_day_min/max` and `s_heart.min_today/max_today`). It also sets `s_heart.spo2_last = 0`, because Breathe passes spo2 as 0 (`nos/svc.c:625-643`, especially `:642`). It does **not** add to the 144-entry history. `heart_tick` rebuilds the history only from `hal_hr_history_samples` (`nos/svc.c:586-617`, `:592`).

### Engine

- `nos_keep_awake()` on every INTRO and SESSION tick (`fit_breathe.c:301`, `:326`).
- `nos_feedback(FB_TAP)` on disc, AGAIN and DONE taps, and `FB_TICK` on a pill change (`:558-565`).
- `nos_pop()` on DONE (`:565`).

### RAM only

- `static B` (`fit_breathe.c:59-89`): state, timers, HR integrals, and the average.
- `static s_min_i` (`fit_breathe.c:92`): the selected duration. It resets to index 1 (3 MIN) on every boot.

Nothing is written to NVS, SD or prefs by this screen.

## Background behaviour

Nothing runs in the background for Breathe. The rules below are from source.

- **Foreground only.** `tick_screens` ticks only the top screen when no transition is running (`nos/nos.c:2101-2110`). Breathe's `tick` and `draw` run only then, and only while the display is ON. The session clock `B.t` is the sum of frame `dt`, not wall-clock time.
- **Display does not auto-sleep during INTRO or SESSION.** Those ticks call `nos_keep_awake()` (`fit_breathe.c:301`, `:326`). The sleep policy checks the top screen's `SCR_KEEPAWAKE` flag, which Breathe does not set (`fit_breathe.c:574-575`, flags = 0), and the idle timer (`firmware/main/nos_main.c:2080-2085`, `nos/power_policy.h:18-19`). OUTRO and SUMMARY do not keep the screen awake (`fit_breathe.c:359-377`).
- **Display off** (`PWR_OFF`): the loop runs `nos_bg_tick` at 1 Hz and does not tick the top screen (`firmware/main/nos_main.c:2090-2094`). A session would freeze, with no haptics and no HR sampling. In practice the display does not switch off during a session, except by the paths below.
- **AOD or sensor mode** (default `sensor_idle = 1`, `nos/prefs.c:159`): `go_state` calls `nos_home_now()` (`nos_main.c:705`). That pops Breathe with a normal leave, so `hr_stop` runs and the session is lost.
- **Covered** by a pushed screen (ring, power sheet): the top changes, so Breathe gets no tick and its clock pauses. `breathe_leave` keeps `hr_on` (`fit_breathe.c:224-228`). The strap lease then idles out after 8 s with no getter (`firmware/main/ext/ext_hr.c:238`, `NOS_EXT_IDLE_MS` in `nos/hal_ext.h`). On reveal, `breathe_enter` re-requests the stream (`fit_breathe.c:210-214`).
- **Left** by DONE, a rightward swipe, a short side-button press (`nos/nos.c:952`), or AOD collapse: `breathe_leave` calls `hr_stop`, and the session is discarded.
- **Power cost**: the screen stays on for the whole session. The strap starts with about 6.2 mA per LED (`firmware/main/ext/ext_hr.c:102`), but only while a lease is held. See [[Power and battery]].
- **Strap driver, not Breathe**: task `ext` runs `hr_service()` every loop (`firmware/main/ext/ext_sys.c:144-170`, `:159`). It is created at `ext_sys.c:186-188` at priority 0, with a 4 KB stack in PSRAM and no core affinity. While a lease is held, the FIFO drains every 120 ms (`ext_hr.c:230-307`). When the lease expires it stops the sensor (`ext_hr.c:251-255`).
- **Samples to sports**: while the stream is on, `hr_service` emits `SP_PPG` and `SP_HEART` records (`ext_hr.c:284`, `:296`). Whether they reach the journal is decided in `nos/sports.c` and is outside this card. See [[Sports and workouts]].
- **Background HR sampler, not Breathe**: one measurement per 10 min, at most 40 s each (`ext_hr.c:34-35`, `:246-249`), first one 2 min after boot (`:322`). It writes the 144-entry RAM history, which Breathe never reads or writes.

Scheduling context is in [[Engine and scheduling]]. The services that keep running with the screen off are listed in [[Services hub]].

## Persistence

Breathe persists nothing.

| Item | Where | Survives leave | Survives reboot |
|---|---|---|---|
| Session clock, phase, HR integrals, average | `static B` (`fit_breathe.c:59-89`) | No (`breathe_enter` resets it) | No |
| Selected duration | `static s_min_i` (`:92`) | Yes, until reboot | No, resets to 3 MIN |
| Last HR, day min/max, last SpO2 (written by Breathe) | `static svc_heart_t s_heart` (`nos/svc.c:72`) | Yes | No, RAM only |
| Strap HR 144-entry history | RAM (`ext_hr.c`) | Yes | No |

There is no NVS key, SD path, prefs key or session log for Breathe. Other services use NVS keys such as `"nosact"` and `"nosalarm"` (`nos/svc.c:499`, `:728`), but none for Breathe. Storage details are in [[Storage and persistence]].

## Navigation

- **Opened from**: the launcher tile "Breathe" (`nos/apps/registry.c:19`), pushed with `TX_IRIS`. A double press reopens the last app opened (`nos/apps/registry.c:40-41`). The sim screen list also names it (`sim/nos_sim.c:73`). Shell context is in [[Shell and faces]].
- **Opens**: no screens. A long side-button press pushes the power sheet over it (`nos/nos.c:953-955`). That covers Breathe and pauses its session.
- **Leaves to**: DONE pops to whatever opened it (`fit_breathe.c:562-566`). A rightward drag starts a back transition (`nos/nos.c:852-856`). A short side-button press calls `nos_home()` (`nos/nos.c:952`). AOD collapses to home (`nos_main.c:705`).
- **Other screens that read its data**: the Heart app reads `bpm_last` for its LAST reading and age (`nos/apps/fit_heart.c:204`). The home face takes `svc_heart()->bpm_last` with no age check (`nos/apps/sys_home.c:473`). The phone `act` packet sends `bpm_last` when realtime HR is on (`nos/svc.c:1145`).
- **Not referenced** by any home widget, complication, alarm or notification path (`scr_breathe` appears only in `registry.c:19`, `nos_apps.h:44` and the sim list).

## Gaps

- **[high] Finished summary is collapsed to home by the idle timer.** `ST_SUMMARY` never calls `nos_keep_awake` (`fit_breathe.c:374-377`). Breathe's flags are 0 (`:574-575`). After `sleep_s` with no input (default 15 s, `nos/prefs.c:149`, minimum 5 s at `:168`), the policy enters SENSOR mode (`nos_main.c:2080-2085`, `nos/prefs.c:159`). That calls `nos_home_now()` (`nos_main.c:705`), which pops the summary. The minutes, breath count and average BPM are gone, and the next open starts at START. Source-derived, not measured on the watch.
- **[high] No end, pause or confirm; leaving discards the session silently.** `breathe_input` consumes only the disc, pills, AGAIN and DONE (`fit_breathe.c:538-572`). A rightward drag starts a back transition on any screen without `SCR_NOBACK` or `SCR_HDRAG` (`nos/nos.c:852-856`), and Breathe sets neither. A side-button press pops the screen (`nos/nos.c:952`). Both paths hit `breathe_leave` and `hr_stop` (`fit_breathe.c:224-230`). The summary and HR record happen only at natural end (`:348-356`). One stray swipe or press ends a 1-5 minute session with no record.
- **[medium] Nothing is saved.** There are no session logs, mindful-minute totals, streaks or last-used duration. `s_min_i` is RAM only (`fit_breathe.c:92`), so every boot starts at 3 MIN. There is no NVS or SD write (`docs/os-map/systems/Health and activity.md:110` says the same).
- **[medium] Bare 1.43-B shows HR as unavailable and gives no fix.** There is no strap, so `EXT_HR` is absent (`nos/hal_ext.h:4-12`, `fit_breathe.c:162`, `:213`). The summary says "HEART RATE UNAVAILABLE" and no hint to add a strap. "HR COVERAGE 0%" is still printed as if the sensor had failed (`fit_breathe.c:486-489`).
- **[medium] Haptics are the only non-visual cue and need a strap motor.** `hal_haptic` drives IO16 through LEDC, which sets `EXT_HAPTIC` once the driver's setup succeeds, with no probe (`firmware/main/ext/ext_haptic.c:102-109`). The header lists the motor as a strap-module pin (`nos/hal_ext.h:1-12`, `firmware/main/ext/ext_priv.h:33`). `firmware/main/pins.h` has no haptic entry. The repo does not show whether the metal-case B has an on-board motor. B has no built-in audio (CLAUDE.md hardware table). Without a motor the session is visual only.
- **[medium] The average overwrites shared heart state and clears SpO2.** Breathe passes `spo2 = 0`, so `s_heart.spo2_last` is cleared (`fit_breathe.c:353`, `nos/svc.c:642`). The Heart app's SpO2 disappears after a breathing session (`fit_heart.c:204`). `bpm_last` becomes the number the home face shows, with no age check (`sys_home.c:473`), and the phone `act` packet sends it when realtime HR is on (`svc.c:1145`). A session mean is therefore shown as if it were a live reading.
- **[low] No live readout during the session.** Breathe shows only the phase word (`fit_breathe.c:459-465`) and the dial. There is no time remaining, breath count or live BPM. The summary is shown only at the end.
- **[low] Pace and length are fixed.** The pattern is 4-2-6 at 5 breaths per minute (`:28-31`, `:201`). Length is 1, 3 or 5 minutes only (`:91`). There is no slower or custom pattern. The hold has no haptic cue (`:332-336`).
- **[low] No reminders, scheduled sessions or home complication.** Nothing in the alarm, notification or widget code refers to breathing (grep of `scr_breathe` and `breathe`: only `registry.c:19`, `nos_apps.h:44`, `sim/nos_sim.c:73`). Compare the "Breathe" tile in [[Shell and faces]].
- **[low] Average can be dropped with no message when the clock is untrusted.** `svc_heart_sample_epoch` returns 0 with no trusted clock (`nos/svc.c:619-623`). `svc_heart_record_at` then rejects the record if a stamped reading already exists in this boot (`:628-631`). The user sees no message. This needs a clock reset in the same boot, so it is rare.
- **[low] Comment drift: "Holds are truly still (zero repaint)".** The header says so (`fit_breathe.c:12`). The disc holds still, but `prepare_session` lights the dial by elapsed time with no phase gate (`:256`), so the leading capsule still advances during holds (`draw_dial` `:146-154`).
- **[low] Test coverage is narrow.** Only the HR integration has a host test (`tests/test_health_regressions.py:153-184`). Nothing covers the state machine, the idle timeout or the back and button leaves. This card did not run any test.

## Related

[[Services hub]] · [[Health and activity]] · [[Sports and workouts]] · [[Power and battery]] · [[Storage and persistence]] · [[Hardware and drivers]] · [[Connectivity and voice]] · [[Shell and faces]] · [[Engine and scheduling]]
