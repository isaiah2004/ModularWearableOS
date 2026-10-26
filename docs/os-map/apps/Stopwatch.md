---
title: "Stopwatch"
id: app-stopwatch
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Stopwatch

Source: `nos/apps/time_stopwatch.c` (684 lines). Read-only source review on 2026-10-23. Nothing was built, flashed or run on a board, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status is from source reading only.

## Summary

Stopwatch is the NOS start, stop, lap and reset screen (`scr_stopwatch`, `nos/apps/time_stopwatch.c:684`; launcher entry `nos/apps/registry.c:23`). It owns no clock. The running state is the service struct `svc_stopwatch_t`, held in static RAM by `nos/svc.c` (`s_sw`, `:77`; functions `:912-938`). Its time base is `hal_millis()`, a monotonic millisecond count from `esp_timer` (`firmware/main/hal_esp32.c:68-69`, `:111-114`). The screen copies that struct every frame, draws it, maps taps to two pills, and keeps only animation state.

Nothing is persisted. A reboot, crash, power loss or OTA restart zeroes the stopwatch. Leaving the screen, screen-off and AOD do not stop or lose the timing, because `svc_tick()` refreshes the elapsed value from the engine hook (`nos/svc.c:1324`, called via `nos/apps/registry.c:62-66`).

Hardware: the 1.43-B display and the FT3168 touch. Feedback goes through `hal_haptic` and `hal_sfx`, whose B-board wiring is unverified (see Gaps). It needs no IMU, RTC, SD card, radio or audio. The wall clock is not read, so an RTC or time-sync change does not disturb a running stopwatch.

Build: the file is compiled into the NOS image by the `${NOS_SRCS}` glob (`firmware/main/CMakeLists.txt:15`, `:54`). The old `core/apps/app_stopwatch.c` is not built (`firmware/main/CMakeLists.txt:12-13`). Its "long-press to reset" comment does not describe the NOS screen.

Related: [[Utility apps]], [[Services hub]], [[Engine and scheduling]], [[Shell and faces]], [[Timer]].

## Features

- **Digits.** `mm:ss` in tall dot-matrix glyphs with a `.t` tenths cell (`nos/apps/time_stopwatch.c:565-578`). From one hour the hero switches to `h:mm:ss` (`:237`). Digits morph when they change (`dgm_set`, `dgm_draw`, `:103-134`). Display resolution is 0.1 s; the stored value is 1 ms.
- **Layout change on first lap.** The digits sit at y 186 solo and move to y 100 once a lap exists (`:157-158`, `:313-316`). The move is a cut with the rows fading in after it (`:165-167`).
- **Bezel track.** 60 slanted dashes at r 203-219 px. The seconds elapsed in the current minute are lit. Odd minutes invert the lit run so the minute rolls over without a cut (`time_sw_lit`, `:77-82`). A red second hand makes one turn a minute (`:359-362`, `:559-562`). On RESET the hand flies back to 12 in a cut (`:344-362`).
- **Start/Stop pill (right).** Labels `START` (idle), `STOP` (running) and `RESUME` (paused) (`sw_labels`, `:257-262`). A tap starts or stops (`sw_input`, `:667-669`).
- **Lap/Reset pill (left).** `LAP` while running, `RESET` while paused, and a dimmed `LAP` while idle (`:257-262`, `:587-589`). A tap while running records a split (`:672-674`). A tap while paused clears everything (`:675`).
- **Lap list.** Window y 194-302, about three rows (`:149-150`). The live row shows `LAP nn` and the current lap time. Completed laps follow newest first (`sw_row`, `:391-417`). Once there are at least two laps, the best lap is white and the worst is grey (`sw_snapshot`, `:246-254`; `sw_row`, `:409-411`).
- **Scrolling.** A vertical drag scrolls the list by whole rows with fling and snap (`sw_input`, `:641-656`; `sw_fling_snap`, `:617-629`). Horizontal drags are not consumed (`nos/apps/time_stopwatch.c:684`, flags 0).
- **Stopped state.** A `PAUSED` label appears above the digits when stopped with the lap list hidden (`:573-575`).
- **Feedback per action.** START gives `FB_TOGGLE_ON`, STOP `FB_TOGGLE_OFF`, LAP and RESET `FB_TAP`, a lap at the cap `FB_LIMIT`, and LAP while idle `FB_ERROR` (`:668-675`). Mapping: `nos/nos.c:557-575`.
- **Lap cap.** 30 laps (`SVC_LAPS`, `nos/svc.h:148`). The 31st Lap tap is refused (`time_stopwatch.c:673`; `nos/svc.c:928`).
- **Keep on.** While running and on top, the screen does not auto-sleep (`time_stopwatch.c:343`).

## Reads

Service getters:
- `svc_stopwatch()` (`nos/svc.c:913`, declared `nos/svc.h:155`). Called by `time_sw()` (`nos/apps/time_stopwatch.c:38-44`) and by `sw_poll()` (`:64-70`). `sw_snapshot()` copies the whole struct each frame, `S.sw = *time_sw()` (`:231-234`). Fields used: `running`, `elapsed_ms`, `nlaps`, `laps[0..29]`.
- `time_sw()` is also read by the Home Timer glance: `wg_mode_now()` tests `running` (`nos/apps/time_timer.c:1030-1031`), and the glance tick sets `W.sw_ms = time_sw()->elapsed_ms` (`:1060`).

HAL and platform:
- `hal_millis()`. Read directly only in the fallback (`time_stopwatch.c:42`, `:55`). The live writers read it in `nos/svc.c:917`, `:923`, `:929`, `:937`. Within a UI frame it returns the value captured by `hal_frame_begin()` (`firmware/main/hal_esp32.c:113`, `:132-137`), which the frame calls at `nos/nos.c:2321`.
- `nos_prefs.haptics` and `nos_prefs.sound` are read inside `nos_feedback()` (`nos/nos.c:562-574`), not by this screen. The screen reads no prefs key.

Engine and UI helpers:
- `nos_enter_t()` and the `nos_t` clock (`nos/nos.h:188-189`).
- Motion helpers `m_*`, `mtw_*`, `msc_*` (`nos/motion.h`) and `ui_*` draw helpers (`nos/ui.h`).
- Geometry constants `UI_BTN_*`, `UI_ROW_*`, `UI_CX` (`nos/ui.h:46-71`).
- Draw helpers owned by Timer: `time_slant_xy`, `time_slant` (`nos/apps/time_timer.c:89-103`).
- `SVC_LAPS` (`nos/svc.h:148`).

Not read: NVS, SD, the wall clock, IMU, battery, phone or Gadgetbridge state, and the activity or sports services. A case-insensitive grep of `nos/`, `core/`, `firmware/main/`, `sim/`, `tests/` and `tools/` found no stopwatch reference outside the files cited in this note.

## Writes

Service mutators in `nos/svc.c`. They are called only from `time_sw_cmd()` (`nos/apps/time_stopwatch.c:46-53`), which `sw_input()` calls on tap (`:663-678`):
- `svc_sw_start()` (`svc.c:914-919`): sets `running = 1` and `s_sw_t0 = hal_millis() - elapsed_ms`, so a resume continues from the frozen value.
- `svc_sw_stop()` (`:920-925`): freezes `elapsed_ms` and clears `running`.
- `svc_sw_lap()` (`:926-931`): while running and below 30 laps, appends `elapsed_ms` to `laps[nlaps++]`.
- `svc_sw_reset()` (`:932-938`): zeroes the struct, keeps the `running` bit, and resets `s_sw_t0`.

Service tick: `svc_tick()` sets `s_sw.elapsed_ms = now - s_sw_t0` on each call while running (`nos/svc.c:1324`). This is the only writer of elapsed between taps.

Engine side effects:
- `nos_keep_awake()` (`time_stopwatch.c:343`) writes `s_last_input_ms` (`nos/nos.c:585`) on each UI frame while running on top.
- `nos_feedback()` (`nos/nos.c:557-575`) drives `hal_haptic` and `hal_sfx`.

Screen-local state: `S` (`time_stopwatch.c:171-203`), including the ghost rows `S.gh`, the list scroll `S.sc` and the colour step `S.dim`. Statics `SL`, `SL_base`, `s_svc`, `s_probe` (`:34-36`) belong to the fallback. `s_trk_skip` and `s_trk_ok` (`:527-528`) are a cache. `s_seen` (`:266`) is the re-entry guard. None of this is persisted.

Not written: no prefs key, NVS namespace, SD file, notification, phone or Gadgetbridge message, Activity log or Sports log.

## Background behaviour

The screen has no task of its own. Its clock is `svc_tick()`, the engine hook `nos_bg_tick` (`nos/apps/registry.c:62-66`), which runs on the UI task. That is `app_main` on core 0 (`firmware/main/nos_main.c:1899`; `firmware/sdkconfig:1710` pins the main task to CPU0). The screen's own `sw_tick` runs as the top screen, or as the under or over screen during a transition (`nos/nos.c:2101-2110`).

- **Display on.** `svc_tick` runs once per UI frame, before `tick_screens` (`nos/nos.c:2341`, `:2347`). Frames pace to `NOS_PRESENT_FPS` 60 (`nos/render_config.h:13`), so a read can lag real time by up to one frame, about 17 ms. The logical motion clock is `NOS_FPS` 40 (`nos/render_config.h:8`).
- **Display off (PWR_OFF).** The host loop calls `nos_bg_tick(1.0f)` once per second (`firmware/main/nos_main.c:2091-2094`). The stopwatch's tick and draw do not run. Time stays correct because elapsed is computed from the absolute base (`nos/svc.c:1324`). `elapsed_ms` can be up to 1 s stale, but nothing reads it while off.
- **AOD and sensor idle.** A 1 Hz `nos_frame_run` (`firmware/main/nos_main.c:2114-2117`) runs the services and the top screen's tick once per second.
- **Auto-sleep.** While running on top, `sw_tick` calls `nos_keep_awake()` every frame (`time_stopwatch.c:343`). The idle timer fires only when `awake > sleep_s` with no keep flag (`firmware/main/nos_main.c:2080-2086`; `nos/power_policy.h:18-19`). So it never fires while the stopwatch runs on screen, and the watch never reaches AOD or OFF from that path. Popping the screen ends the keep, and normal sleep resumes.
- **No keep flag.** `scr_stopwatch.flags` is 0 (`time_stopwatch.c:684`). The stopwatch does not use `SCR_KEEPAWAKE` (`nos/nos.h:64`), although that comment names the stopwatch as a user.
- **Survives:** closing the app (state lives in svc), screen off, AOD, and the Home Timer glance, whose tick reads `time_sw()` (`time_timer.c:1030`, `:1060`). The glance switches to STOPWATCH mode while the stopwatch runs (`:1027-1033`).
- **Does not:** wake the display, raise a notification, alert at a lap or at the cap, drive AOD, or survive a reboot.

## Persistence

- None. `s_sw` is zero-initialised static RAM (`nos/svc.c:77`). No store call appears in the stopwatch functions (`svc.c:912-938`). The screen has no prefs key, no NVS namespace and no `/watch/` path on SD.
- Survives leaving and reopening the app, screen-off and AOD, and the Home glance.
- Lost on reboot, crash, brown-out, power loss or OTA restart. A running session restarts at 0 and all laps are gone.
- See [[Storage and persistence]] for the NVS and SD layers the other apps use.

## Navigation

Opened from:
- **Launcher.** "Stopwatch" is entry 14 of `nos_app_list` (`nos/apps/registry.c:23`). The launcher opens entries with `nos_open` or `nos_open_icon` (`nos/apps/sys_launcher.c:968-969`). `nos_open` records the app for the double-press reopen, then plays the open cue and pushes it with an iris transition (`nos/apps/registry.c:45-53`).
- **Home Timer glance.** While a stopwatch runs, the glance shows STOPWATCH (`nos/apps/time_timer.c:1027-1033`). A tap calls `nos_open(&scr_stopwatch, x, y)` (`:1194`). See [[Timer]].
- **Last-app double press.** `s_last_app` is set when a registry screen opens (`nos/apps/registry.c:42`, `:47`) and read by `nos_last_app()` (`:43`; `nos/apps/sys_launcher.c:139`).

Opens: nothing. The screen has no sub-screens and pushes none.

Leaves:
- **Swipe back.** Allowed, since no `SCR_NOBACK` flag is set (`time_stopwatch.c:684`).
- **Side button.** A press goes home. `sw_input` returns 0 for `IN_BTN`, so `dispatch_btn` calls `nos_home()` when the stack is deeper than one (`nos/nos.c:952-955`). A long press opens the power screen (`:953-955`).

Touch map: pills at y 372 (`nos/ui.h:62`), centred at x 154 (left) and x 312 (right) (`nos/ui.h:60-61`). `sw_hit` accepts |y - 372| <= 50 and |x - centre| <= 74 (`time_stopwatch.c:604-610`). The lap list window is y 194-302 (`:149-150`).

## Gaps

- **medium. Running time and laps are not persisted.** `s_sw` is static RAM (`nos/svc.c:77`), and the stopwatch functions never store it (`nos/svc.c:912-938`). A reboot, crash or OTA restart restarts at 0. A long session is lost.
- **medium. The display stays on while a stopwatch runs.** The screen keeps the watch awake every frame (`time_stopwatch.c:343`), and the idle timer is held off (`firmware/main/nos_main.c:2080-2086`). A forgotten running stopwatch keeps the panel on until the user leaves the screen. This is deliberate per the `nos/nos.h:64` comment, but it costs battery. See [[Power and battery]].
- **medium. Haptic and sound cues are unverified on the 1.43-B.** `hal_haptic` drives a coin motor on IO16 (`firmware/main/ext/ext_haptic.c:1-3`). The B pin map in `firmware/main/pins.h` defines no haptic pin. IO16 appears there only as the C board's touch reset (`pins.h:24`). CLAUDE.md says the B has no built-in audio. START, STOP, LAP and the cap cue may therefore be silent on the B. This is the only tactile confirmation the screen has. See [[Hardware and drivers]].
- **medium. No phone or activity sync.** Laps and sessions are not written to Activity, Sports or Gadgetbridge. The case-insensitive grep found no stopwatch reference in `nos/gb.c` or `nos/apps/fit_workout.c`. There is no export and no history. See [[Health and activity]], [[Sports and workouts]] and [[Connectivity and voice]].
- **low. The lap cap is silent.** At 30 laps, Lap gives only a tick haptic (`time_stopwatch.c:673`; `nos/nos.c:562`; no sound, `:567`). The live row keeps counting as `LAP 31` (`:244`), and that lap can never be recorded. Nothing on screen says so.
- **low. A dimmed LAP buzzes like an error.** While idle the left pill reads a dimmed `LAP` (`:260`, `:589`). A tap calls `FB_ERROR` (`:671`), which maps to the heavy haptic (`nos/nos.c:562`). A control that looks disabled fails loudly.
- **low. RESET is one tap and clears everything.** `svc_sw_reset` zeroes the struct including all laps (`nos/svc.c:932-938`), called from one tap while paused (`time_stopwatch.c:675`). There is no confirmation, so 30 laps can be lost to a stray tap.
- **low. Start, stop and lap carry up to one frame of latency.** The service stamps them with `hal_millis()` (`nos/svc.c:917-937`). Inside a UI frame that value is frozen at frame start (`firmware/main/hal_esp32.c:132-137`). `hal_frame_begin()` runs before input (`nos/nos.c:2321`, `:2331`). A press can therefore be stamped up to one frame, about 17 ms, after it happened. The offset is fixed, not drift.
- **low. Thin lap list.** Each row shows a lap time only. There is no cumulative total, no average and no fastest/slowest summary beyond the colours (`time_stopwatch.c:401-417`). No hundredths and no auto-lap.
- **low. Running state is visible only on Home.** There is no launcher badge, no status-bar item and no AOD or complication. `nos/apps/sys_launcher.c` and `nos/apps/sys_home.c` have no stopwatch reference. A running stopwatch shows only through the Timer glance or when opened.
- **low. No control outside the screen.** The side button does not start or lap, and only goes home (`nos/nos.c:952-955`). No Control Center tile (`nos/apps/sys_control.c`) or Chat command (`nos/apps/con_chat.c`) references the stopwatch.
- **low. Stale comments and a dead fallback.** `time_stopwatch.c:9-11` says svc "is still the link stub". The NOS build compiles the real `svc.c` (`firmware/main/CMakeLists.txt:15`, `:54`). `svc_sw_start` sets `running` at once, so `s_svc` becomes 1 at the first `sw_poll` (`time_stopwatch.c:64-69`), and the `SL` path (`:34-63`) never serves a read. `nos/nos.h:64` says the stopwatch uses `SCR_KEEPAWAKE`, but it does not (`time_stopwatch.c:684`). Maintenance only.
- **Not a gap:** there is no placeholder or fake data on the screen. Every value it shows comes from svc or the frame clock.

[[Services hub]] · [[Engine and scheduling]] · [[Power and battery]] · [[Storage and persistence]] · [[Hardware and drivers]] · [[Connectivity and voice]] · [[Shell and faces]]
