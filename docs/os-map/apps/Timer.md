---
title: "Timer"
id: app-timer
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Timer

Source: `nos/apps/time_timer.c` (1212 lines). Read-only source review; nothing built, flashed or run on a board, and the serial port was not touched. Evidence is `repo-relative/path:line`.

## Summary

Timer is the NOS countdown app. One file holds four surfaces: the setup screen with six presets and a CUSTOM pill (`nos/apps/time_timer.c:781`), the drum picker "TIMER SET" (`:863`), the run view with a 60-dash ring, and the "TIME'S UP" ring screen (`:987`). The same file holds the home-pager glance `wg_timer` (`:1212`, placed in `nos/apps/sys_home.c:203`).

The countdown does not live in the app. It is `svc_timer_t` in the services hub: one RAM struct with state idle/running/paused/ringing, `total_ms` and `left_ms` (`nos/svc.h:136-146`, `nos/svc.c:74-76`). `timer_tick()` runs inside `svc_tick()` (`nos/svc.c:894-910`, called at `:1325`), so a running timer counts down and rings with the app closed and the screen off. The app only issues start, pause, resume, cancel and add, and draws the state it reads back.

Nothing is persisted. The timer, its ring and the custom-picker value are lost on reboot, restart and POWER OFF. Screen-off SLEEP keeps them.

Hardware: the countdown needs only the MCU clock. `hal_millis()` is `esp_timer_get_time()/1000` (`firmware/main/hal_esp32.c:68-70`, `:111-114`). The screen (CO5300-class 466 x 466 panel per CLAUDE.md; pins at `firmware/main/pins.h:37-46`) and touch (FT3168 at 0x38, `pins.h:50-52`) are needed to use it. The ring's alert needs the strap: a haptic motor on GPIO16 through LEDC (`firmware/main/ext/ext_priv.h:33`) and a speaker through I2S on GPIO1/2 with amp pins on GPIO5/6 (`ext_priv.h:34-38`). B has no built-in audio (`firmware/main/pins.h:64`, `BOARD_HAS_AUDIO 0`). No IMU, RTC or SD is needed for the countdown. The widget's NEXT ALARM mode does need the wall clock (`nos/svc.c:770-771`).

Status is partial, from source reading only. The timer loop, presets, custom picker, run view and glance are complete for one timer. The alert output on the B build is not verified (see Gaps). The ring ignores the Sound and Haptics settings.

Doc drift:
- `nos/apps/time_timer.c:8-10` says the service is a stub and the local fallback runs the timer. The real service is in `nos/svc.c`, and the fallback is dead code.
- `docs/os-map/systems/Utility apps.md:15` says there is no deep sleep. `firmware/main/nos_main.c:809-811` deep-sleeps on POWER OFF.
- `docs/os-map/systems/Utility apps.md:36` says the Timer writes "the ring sound and haptic only". It also writes service timer state and diagnostic trace records (see Writes).
- `nos/apps/time_timer.c:913-914` says the ring "screen stays until dismissed". The service ends it after 60 s (`nos/svc.c:905-907`).

## Features

- **Presets.** Six pills, 1 / 3 / 5 / 10 / 15 / 30 MIN, in a 3 x 2 grid (`nos/apps/time_timer.c:473`, `:476-477`). A tap starts the timer at once and switches to the run view (`:753-758`). There is no confirm step.
- **CUSTOM.** Pushes the picker (`:759-761`). Two drums: minutes 0-99 and seconds 0-59 (`:793-794`), default 10:00 (`:792`). START with 00:00 gives an error feedback and does nothing (`:855`). Otherwise it starts the timer and pops back (`:857-858`). Maximum is 99 min 59 s.
- **Run view hero.** MM:SS, or H:MM:SS from one hour. Seconds round up, so 4:59.2 shows 5:00 (`:398-404`). A total label reads "N SEC", "N MIN", "N MIN N SEC" or "N H N MIN" (`:405-412`).
- **Ring.** 60 slanted dashes drain anticlockwise toward 12 o'clock, showing the remaining share of the total (`:588`, `:649`, `:657-663`).
- **Last 10 seconds.** A red needle at the exact fractional end, and the leading digits dim (`:655-661`, `:674`). This shows only while running (`:589`).
- **Pause and resume.** The centre disc toggles, with an icon and colour step over 150 ms (`:689-691`, `:591-592`, `:762-764`). The total label crossfades to PAUSED (`:666-671`).
- **Cancel.** The left cross discards the timer and returns to setup (`:765-770`). No confirm.
- **+1 MIN.** The right pill adds 60 s to a running or paused timer (`:771-774`; `nos/svc.c:888-893`).
- **TIME'S UP ring.** Pulsing dot rings, a fixed "00:00" hero, STOP on the left and "+1 MIN" on the right (`:916-935`). The hardware side button is STOP on this screen (`:983`). "+1 MIN" cancels and starts a new 60 s timer (`:949-959`).
- **Home glance `wg_timer`.** Mode priority is timer (states 1-3), then a running stopwatch, then the next enabled alarm, then quick start (`:1027-1034`). Quick start has three circles, 3 / 5 / 10 MIN (`:997`, `:1129-1143`), each starting at once (`:1187`). A tap on the timer mode opens Timer (`:1193`), the stopwatch mode opens Stopwatch (`:1194`), the alarm mode opens Alarms (`:1195`). A tap elsewhere or on quick start opens Timer (`:1190`).
- **Tap feedback.** Taps call `nos_feedback` (`:754`, `:766`, `:772`, `:855-856`, `:974`), which honours the Haptics and Sound prefs (`nos/nos.c:570-572`).

## Reads

Service getters:
- `svc_timer()` (`nos/svc.c:856`, declared `nos/svc.h:141`), read in `tm_get` (`nos/apps/time_timer.c:337-350`, svc branch `:348-349`) and in `tm_poll` (`:390`). Fields: `state`, `total_ms`, `left_ms`. `tm_get` runs on every tick of the app, the ring and the glance (`:558`, `:574`, `:756`, `:768`, `:881`, `:900`, `:1039`). The picker calls only `tm_poll` (`:804`).
- `svc_stopwatch()` through `time_sw()` (`nos/apps/time_stopwatch.c:38-42`). Glance only: `running` (`nos/apps/time_timer.c:1030-1031`) and `elapsed_ms` (`:1060`).
- `time_alarm_next()` (`nos/apps/time_alarm.c:118-132`), which reads `svc_alarm_next()` (`nos/svc.c:768-782`). Glance only (`nos/apps/time_timer.c:1032`, `:1066-1069`). Any enabled alarm with a future occurrence in the next 8 days hides quick start.

HAL and clock reads:
- `hal_millis()` in the fallback only (`nos/apps/time_timer.c:342-343`, `:363`, `:370`). Implementation: `firmware/main/hal_esp32.c:111-114`. The value is frozen for each frame (`hal_esp32.c:132-138`), so one frame's ticks see the same time.
- `hal_get_datetime()` is reached only through `svc_alarm_next()` (`nos/svc.c:770-771`).

Preferences:
- `nos_prefs.sound` is read only in the fallback ring path (`nos/apps/time_timer.c:887`), which is dead on the current service. The service ring path does not read it for timers (`nos/svc.c:701`).
- `nos_prefs.haptics` is not read anywhere on the Timer or service ring path.

Engine and screen state (not data): `nos_t`, `nos_enter_t()`, `nos_top()`, `nos_transitioning()`, `nos_touch_travel()` (`nos/apps/time_timer.c:1050`), and the widget blur snapshot `pblur_baking`, `pblur_busy`, `hal_big_alloc` (`:1148-1152`).

Input: the touch and side-button events the engine delivers as `nos_input` (`:738-779`, `:960-986`).

## Writes

Service mutators (the only writes to timer state):
- `svc_timer_start(ms)` (`nos/svc.c:857-865`). Called from presets (`nos/apps/time_timer.c:755`), quick start (`:1187`), CUSTOM START (`:857`) and ring "+1 MIN" (`:955`). Sets `total_ms`, `left_ms`, `s_timer_end = hal_millis() + ms`, `state = 1`, and bumps `s_timer_generation`.
- `svc_timer_pause()` (`nos/svc.c:867-873`), `svc_timer_resume()` (`:875-880`), `svc_timer_cancel()` (`:882-886`), `svc_timer_add(ms)` (`:888-893`). Called from `tm_pause`, `tm_resume`, `tm_cancel` and `tm_add` (`nos/apps/time_timer.c:359-384`).
- `svc_timer_cancel()` also ends the ring. It is called by ring STOP (`nos/apps/time_timer.c:945`), ring "+1 MIN" (`:954`), the 60 s timeout (`nos/svc.c:905-907`) and the ring-leave check (`nos/svc.c:909`).

Diagnostics (side effect of every action):
- Start, pause, resume, cancel, fire and timeout each call `action_trace` (`nos/svc.c:102-107`, called at `:861`, `:870`, `:878`, `:884`, `:899`, `:906`). This goes to `diag_action_*` (`nos/diag_log.c:229-243`) and is queued for the SD diagnostics log under `/sdcard/watch/diagnostics/diag-NN.bin` (`nos/diag_log.c:24`, `:395`). These records hold the action and the state, not the countdown.

Ring output (service side):
- `hal_haptic(HAPTIC_ALARM)` and `hal_sfx(SFX_TIMER_DONE)` every 2 s (`nos/svc.c:699-702`). `hal_haptic_stop()` and `hal_sfx_stop()` run when the ring state ends (`:693`). `nos_keep_awake()` runs (`:704`).

Feedback output:
- `nos_feedback()` for taps, errors and toggles (`nos/apps/time_timer.c:754-774`, `:855-856`, `:974`, `:1185`). This drives `hal_haptic` and `hal_sfx` through `nos/nos.c:570-572`, gated by the prefs.

Keep-awake:
- `nos_keep_awake()` while the run view is on top and running (`nos/apps/time_timer.c:593`), and in the ring (`:890`, `:908`). The service also calls it while any ring is up (`nos/svc.c:704`).

Navigation:
- `nos_push(&scr_timer_pick, TX_PUSH)` (`nos/apps/time_timer.c:761`). `nos_push(&scr_timer_ring, TX_RISE)` in the fallback only (`:395-396`). The service pushes the ring through `ring_screen()` (`nos/svc.c:659-667`, `:909`). `nos_pop()` at `nos/apps/time_timer.c:858`, `:902`, `:947`, `:957`. `nos_open()` to the stopwatch and alarm from the glance (`:1194-1195`).

Local statics (RAM only): `T`, `PK`, `RG`, `W` (`nos/apps/time_timer.c:505`, `:787`, `:870`, `:1001`), the drum state `s_dr[]` (`:121`), and the fallback `TL`, `s_svc`, `s_probe` (`:333-335`).

Not written: `nos_prefs`, NVS, the SD settings files, `hal_store`. A grep of `nos/apps/time_timer.c` finds no `prefs_`, `nvs`, `fopen` or notification call. The only allocation is `hal_big_alloc` for the blur snapshot (`:1150`).

## Background behaviour

- **Who runs it.** `svc_tick()` (`nos/svc.c:1303-1329`) runs the countdown and the ring. It is called from `services_tick()` (`nos/apps/registry.c:63-71`) through `nos_bg_tick`, once per frame (`nos/nos.c:2341`). That is the UI loop in `app_main` (`firmware/main/nos_main.c:1899`), pinned to core 0 (`firmware/sdkconfig:1706-1707`). Its priority is not set in sdkconfig, so it is the ESP-IDF default. The Engine note gives 1 (`docs/os-map/systems/Engine and scheduling.md:11`). The Timer owns no task and no esp_timer. See [[Engine and scheduling]].
- **Cadence.** Screen on: frames at `NOS_FPS` 40 (`nos/render_config.h:8`). Screen off (PWR_OFF): `nos_bg_tick(1.0f)` once a second (`firmware/main/nos_main.c:2091-2094`). AOD and SENSOR: one frame a second (`:2114-2116`).
- **App closed.** `svc_tick` runs whatever screen is on top, so the countdown and the ring continue with the app closed. The app's own `timer_tick` (`nos/apps/time_timer.c:570`) runs only for the top screen (`nos/nos.c:2101-2110`).
- **Accuracy.** The end time is an absolute `hal_millis()` value (`nos/svc.c:864`, `:897`). Tick cadence affects only when the end is noticed: within one frame on screen, and up to 1 s screen off or in AOD.
- **Screen off.** A running timer does not keep the display awake. The run view calls `nos_keep_awake()` only while it is on top (`nos/apps/time_timer.c:593`). The sleep policy checks only the top screen's `SCR_KEEPAWAKE` flag (`firmware/main/nos_main.c:2084`). `scr_timer` has flags 0 (`nos/apps/time_timer.c:781`). The display therefore times out normally during a background countdown. See [[Power and battery]].
- **Ring with the screen off.** On fire, the service sets state 3 and pushes `scr_timer_ring` (`nos/svc.c:896-903`, `:909`). That screen has `SCR_KEEPAWAKE` (`nos/apps/time_timer.c:987-988`, `nos/nos.h:64`). The PWR_OFF loop turns the display on for it (`firmware/main/nos_main.c:2096`), and the AOD loop does the same (`:2121`).
- **Ring cadence.** `ringer_tick()` (`nos/svc.c:688-705`) picks the kind in the order find, call, alarm, timer (`:690-691`). For a timer it sounds HAPTIC_ALARM and SFX_TIMER_DONE every 2 s (`:699-702`) and keeps the display awake (`:704`).
- **Ring length.** `RING_MAX_MS` is 60000 (`nos/svc.c:47`). At 60 s the service cancels the timer (`:905-907`), which stops the buzz (`:693`). The ring screen then pops itself: the app's `ring_tick` pops once the state is not 3 (`nos/apps/time_timer.c:900-904`), and the service pops it after a 2.5 s grace (`nos/svc.c:658`, `:659-675`).
- **Power off and restart.** Nothing runs. POWER OFF calls `nos_host_power(2)` (`nos/apps/sys_power.c:184`, `firmware/main/nos_main.c:751-761`). That deep-sleeps with GPIO0 as the only wake source (`nos_main.c:809-811`). A wake is a boot, and `s_timer` is ordinary RAM. Only the diagnostic records are RTC-retained (`nos/diag_log.c:73-84`). Restart (mode 1) calls `esp_restart()` (`nos_main.c:809`).
- **DND.** The ring ignores Do Not Disturb. The DND check at `nos/svc.c:230` covers banners only.

## Persistence

- Timer state is not persisted. `svc_timer_t`, `s_timer_end` and `s_timer_ring_t0` are file-static RAM (`nos/svc.c:74-76`). The svc timer section (`nos/svc.c:855-910`) and `nos/apps/time_timer.c` contain no save or load call. The services hub says the same (`docs/os-map/systems/Services hub.md:89`, `:207`, `:241`).
- Lost on: reboot, restart, POWER OFF, crash, OTA reboot. Kept across: screen OFF (SLEEP) and AOD.
- The custom-picker value is RAM (`nos/apps/time_timer.c:787`, `:792`, `:797`). It resets to 10:00 after a reboot.
- Presets and quick-start minutes are compiled in (`:473`, `:997`).
- The diagnostic trace of timer actions is queued to SD (`nos/diag_log.c:24`). It records actions, not the countdown.
- The Timer owns no settings. Sound and haptics come from global prefs (`nos/prefs.c:138-140`), with the caveat in Gaps. See [[Storage and persistence]].

## Navigation

Opens from:
- Launcher "Timer" entry (`nos/apps/registry.c:22`), opened by `nos_open()` (`:45-50`, iris transition).
- Home pager widget (`nos/apps/sys_home.c:5`, `:203`). A tap opens Timer (`nos/apps/time_timer.c:1190-1196`).
- Double press of the last app (`nos/apps/registry.c:43`, `nos_last_app()`), when Timer was the last app opened.
- Simulator: `timer` and `timer_ring` in the screen table (`sim/nos_sim.c:79`, `:83`).

Opens (pushes):
- `scr_timer_pick` (TX_PUSH) from CUSTOM (`nos/apps/time_timer.c:761`). START pops it back (`:858`).
- From the glance: `scr_stopwatch` (`:1194`) and `scr_alarm` (`:1195`).
- `scr_timer_ring` (TX_RISE) from the service (`nos/svc.c:909`) and the fallback (`nos/apps/time_timer.c:395-396`).

Ring exits: STOP (`:947`), "+1 MIN" (`:957`), `ring_tick` once the state leaves 3 (`:902`), or the service grace pop (`nos/svc.c:659-675`).

Back: Timer does not set `SCR_NOBACK`, so swipe-back works (`nos/apps/time_timer.c:781`). The ring sets `SCR_NOBACK` (`:987-988`). In the run view the side button goes home, because `timer_input` does not handle `IN_BTN` and unhandled presses go home (`nos/nos.c:952`, `nos/nos.h:42`). On the ring the side button is STOP (`nos/apps/time_timer.c:983`).

## Gaps

1. **Medium. Timer state is lost on reboot, restart and POWER OFF.** Evidence: `nos/svc.c:74-76` (RAM, no save), `nos/apps/sys_power.c:184`, `firmware/main/nos_main.c:809-811` (deep sleep, GPIO0 wake). A 25-minute timer disappears if the watch is powered off or restarted. SLEEP keeps it.
2. **Medium. Alert output on the B build is not verified.** Evidence: `firmware/main/ext/ext_sys.c:177-179` runs `haptic_init()` and `audio_init()` whenever `EXT_BOARD_OK`. `firmware/main/ext/ext_haptic.c:107-109` and `firmware/main/ext/ext_audio.c:343` mark the motor and speaker present without probing ("cannot be probed"). `firmware/main/pins.h:64` has `BOARD_HAS_AUDIO 0`. CLAUDE.md says the external strap "needs its own verified mapping". On a bare B the ring is screen-only, with no indication to the wearer.
3. **Medium. Sound and Haptics settings do not silence the ring, and the two paths disagree.** Evidence: `nos/svc.c:699` calls `hal_haptic(HAPTIC_ALARM)` with no `nos_prefs.haptics` check. `nos/svc.c:701` plays the timer SFX regardless of `nos_prefs.sound`; only the call sound is gated. The fallback path does gate on sound (`nos/apps/time_timer.c:887`). `nos/nos.c:570-572` gate all other feedback. The only silence entry point, `svc_ring_silence()`, is called by the call screen alone (`nos/apps/con_call.c:487`). A wearer who turned Sound or Haptics off still gets the timer alarm.
4. **Medium. One timer at a time.** Evidence: a single `svc_timer_t` (`nos/svc.h:136-140`, `nos/svc.c:74`). Running a second timer means cancelling the first (`nos/apps/time_timer.c:765-770`). No parallel timers.
5. **Low to medium. The ring ends by itself after 60 s, and the code and comment disagree.** Evidence: `nos/svc.c:905-907` cancels after `RING_MAX_MS`. `nos/apps/time_timer.c:913-914` says the screen stays until dismissed. A wearer who is away loses the alert after a minute. The ring shows a fixed "00:00" (`:922`) with no overrun count. Nothing is written to the notification history: no notification call appears in the Timer file or the svc timer section (`nos/svc.c:855-910`).
6. **Low. No labels, and no hour input.** The picker caps at 99 min 59 s (`nos/apps/time_timer.c:793-794`, `:811`). Timers cannot be named. Multi-hour timers can only be built with "+1 MIN".
7. **Low. Fixed presets, and the custom value resets on reboot.** Presets and quick-start minutes are compiled in (`nos/apps/time_timer.c:473`, `:997`). The picker value is RAM only (`:787`, `:792`).
8. **Low. The glance hides quick start whenever an alarm is enabled.** Evidence: `nos/apps/time_timer.c:1029-1033`, `nos/apps/time_alarm.c:118-132`, `nos/svc.c:768-782`. The glance shows NEXT ALARM instead, so the one-tap timer disappears for users with alarms.
9. **Low. Cancel discards the timer with no confirm.** Evidence: `nos/apps/time_timer.c:765-770`. An accidental tap on the cross loses the running timer.
10. **Low. No face complication or Control Centre entry.** `svc_timer()` is called only from `nos/apps/time_timer.c` (grep of `nos/`). A running countdown shows only in the app and on the home glance.
11. **Low. Dead fallback and stale comments.** The fallback `TL` and its probe (`nos/apps/time_timer.c:332-397`) never run with the current service, because `svc_timer_start()` sets state 1 before the first poll. The header (`:8-10`), the ring comment (`:913-914`) and the Utility apps note (`docs/os-map/systems/Utility apps.md:15`, `:36`) are out of date. No runtime impact; misleads reviewers.

Related: [[Services hub]], [[Shell and faces]], [[Engine and scheduling]], [[Power and battery]], [[Storage and persistence]], [[Hardware and drivers]].
