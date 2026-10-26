---
title: "Segment face"
id: face-segment
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Segment face

Source: `nos/faces/face_segment.c` (623 lines). Read-only source review on 2026-10-23, 1.43-B build. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from reading the code only. The device was not exercised.

## Summary

SEGMENT is one of nine watch faces (`nos/faces/faces.c:15-16`; name at `nos/faces/face_segment.c:623`). It is an activity face. The centre shows the time in the scaled hero font, with the date above it and three values below. Three concentric rings of 60 square segments each (6 px thick, 3 px gaps) show steps (outer, r 215), kcal (middle, r 196) and active minutes (inner, r 177) (`face_segment.c:33-38`). The values row reads STP, KCAL and MIN (`face_segment.c:389`, `:402-410`).

Purpose and data model: SEGMENT is a pure consumer of NOS services. It owns no persistent state and writes no prefs or service data. It has one service getter, `svc_activity()` (`face_segment.c:260`). Time, date, battery, charging, the unread flag and the step goal come through the home context `face_ctx` (`nos/faces/face.h:14-27`), which `build_ctx()` fills each frame (`nos/apps/sys_home.c:465-491`).

Hardware for the 1.43-B: the 466 x 466 panel (`core/gfx.h:13-14`), PSRAM for the ring cache and digit masks, the QMI8658 IMU read by `board_imu_raw` for steps (`firmware/main/board.c:398-407`), the battery gauge (`hal_power_get`, `firmware/main/hal_esp32.c:378`), a trusted calendar clock for the time and day rollover, and touch for taps. A phone link is needed only for the unread dot, through `svc_notif_unread()`. The rings need PSRAM and do not fall back (see Gaps).

Status: working on the source read. The rings, time, values, battery chip, unread dot, build-in, wake/AOD and tap all have code paths. The limits are estimates and fixed goals, listed under Gaps.

Where comments or older docs differ from the code:
- `face_segment.c:533` says the no-PSRAM fallback draws "segments as dots". That branch is unreachable. `blit_rings` returns at `face_segment.c:512` while `s_cold` is still set (Gaps).
- The older `docs/polish/faces_r1.md` (lines 14, 29, 73, 181, 185) describes a stale build-in replay, no battery state, no pressed state and up to 4 s live-gain lag. The code has since changed: `enter()` starts from the last shown fill (`face_segment.c:241`); the battery chip is drawn (`face_segment.c:567`, `:595`); press rects exist (`face_segment.c:603-621`, `nos/apps/sys_home.c:1063`); live changes close the gap in about 12 ticks (`face_segment.c:307-313`).
- `face_segment.c:598` says the unread dot is "inside the rings". It sits at (233, 98), about 135 px from the centre, in the empty centre track. No ring is at that radius.
- The kcal ring reads as a measured calorie figure. The code uses a fixed `steps x 0.04` estimate (`nos/svc.c:445`, `KCAL_STEP` at `nos/svc.c:46`).
- `docs/os-map/systems/Shell and faces.md:189` says the SEGMENT warm-up runs from every face. The code confirms this (`nos/faces/faces.c:61`, called from the other faces' ticks such as `nos/faces/face_dot.c:68`).

## Features

- **Steps ring:** one lap of 60 segments equals the step goal (`face_segment.c:266-273`). Past the goal the ring continues in red on a second lap, up to 200 % (`face_segment.c:76`, `:270-271`). Only this ring goes red, so there is one red element on screen (`face_segment.c:339`, `:549`). The other rings stop at full white.
- **Goals:** steps goal is user-set: `nos_prefs.step_goal_100 * 100`, default 8000 (`nos/prefs.c:155`), picked in Settings from 2,000 to 30,000 in 500 steps (`nos/apps/set_settings.c:1936`, `:1952`). Kcal goal 400 and active-minute goal 30 are constants (`face_segment.c:266`).
- **Centre time:** `H:MM` or `HH:MM` in the hero font scaled to 0.66 (`face_segment.c:206`, `:212`, `:328`), 12 or 24 h from `nos_prefs.hour24` (`nos/faces/faces.c:79-100`, default 24 h at `nos/prefs.c:147`). Changed digits roll mechanically at the minute (`face_segment.c:350-367`).
- **Date row:** "SUN 11" style from `ui_weekday3` plus day (`nos/faces/faces.c:107-110`), with a midnight cross-fade (`face_segment.c:259`). The battery chip replaces it while charging or at 20 % or below (`nos/faces/faces.c:582-585`, `face_segment.c:595`). In AOD it shows only below 10 % and not charging (`faces.c:583`).
- **Values row:** steps (K-formatted from 10,000 up, e.g. 12.3K, `face_segment.c:333-337`), kcal (rounded, `face_segment.c:265`) and active minutes, under STP, KCAL and MIN (`face_segment.c:389`, `:402-410`). The values update every tick. They do not animate.
- **Unread dot:** an 8 px dot when any notification is unread (`nos/faces/faces.c:552-553`, `:576-580`). It shows presence only, no count. On this face it sits at (233, 98) (`face_segment.c:598`).
- **Build-in on show and wake:** segments light clockwise from the last shown fill (`face_segment.c:241`, `:237-243`). Each ring takes 9 frames on `EASE_STEADY`, and rings 1 and 2 start one frame behind ring 0 (`face_segment.c:302-306`). The date and values fade in six ink steps (`face_segment.c:235`). The wake from AOD has its own 6-frame path (`nos/faces/faces.c:458-476`).
- **AOD:** the steps ring is drawn as dots for lit segments only (`face_segment.c:370-382`, `:557-562`). The time is outlined and an "N STEPS" label shows (`face_segment.c:582-586`). Kcal and minute rings are not drawn in AOD, and the red lap is suppressed (`face_segment.c:560-561`).
- **Tap:** a touch outside radius 160 (the ring band), or inside the values band, opens the Activity app (`face_segment.c:603-621`). A press highlights a 60 x 60 square at the ring under the finger, or the values band rect (`face_segment.c:608-615`).
- **Picker lite draw:** while the face picker is open, the empty track fades (`face_segment.c:510-531`; `nos/faces/face_kit.h:138-146`).

## Reads

Service getters:
- `svc_activity()` (`nos/svc.h:102`), read once per tick at `face_segment.c:260`. Fields: `steps` (261), `goal` (262), `kcal` (263), `active_min` (264). Every field is produced by `act_publish()` (`nos/svc.c:440-456`).

Home context `face_ctx` (`nos/faces/face.h:14-27`, built in `nos/apps/sys_home.c:465-491`):
- `steps` and `step_goal` (`sys_home.c:470-472`): from `svc_activity()`, falling back to `svc_sys()->steps_today` and `nos_prefs.step_goal_100`.
- `dt` (`hal_get_datetime`, `sys_home.c:467`) for the time, date and AOD drift.
- `t`, `aod`, `enter`, `rec`, `lite`, `unread` (`fk_unread_tick`, `faces.c:552`).
- `battery` and `charging` (`fk_status_shown`, `faces.c:582`), built from `svc_sys()` (`sys_home.c:473-474`; battery from `hal_power_get` at `nos/svc.c:988-995`).
- Not read by SEGMENT: `bpm`, `weather_valid`, `temp_c`, `wx_cond`, `next_alarm_min`. The context carries them and this face ignores them.

Prefs (`nos_prefs`):
- `hour24` (`nos/faces/faces.c:81`, `:89`).
- `step_goal_100` (`nos/apps/sys_home.c:472`; `nos/svc.c:442`).

HAL and drivers (direct or via the shared kit):
- `hal_big_alloc` for the cache struct (`face_segment.c:138`) and digit masks. Under `NOS_PSRAM_TRACKING` this is `nos_mem_alloc` (`core/psram.h:26`). The firmware defines that flag (`firmware/main/CMakeLists.txt:70`).
- `sprite_alloc` for the four quadrant sprites (`face_segment.c:145`; `core/sprite.h:20`, PSRAM through `nos_mem_alloc`).
- `hal_big_free` on the failure path (`face_segment.c:146-147`).
- Through `face_kit`: `hal_millis` (`nos/faces/faces.c:459`); `hal_get_datetime` (`sys_home.c:467`) and `hal_battery_percent` (`sys_home.c:474`) are used by the home context, not by this face directly.
- The face makes no radio, sensor, speaker, haptic, NVS or SD call. `face_segment.c` has no `nos_feedback` or haptic call (grep count 0).

## Writes

- **Persistent stores:** none. The face calls no `svc_*` mutator, no prefs setter and no `hal_store_*` function.
- **RAM statics:** `G` (cache: quadrant sprites, drawn colours, row spans, `face_segment.c:43-49`), `s_shown`, `s_target`, `s_from`, `s_val`, `s_red`, `s_cold`, `s_cold_i` (`face_segment.c:52-55`, `:70`, `:206-219`), plus the animation trackers `s_font`, `s_w`, `s_tm`, `s_date`.
- **PSRAM, one-time:** four quadrant sprites of 233 x 233 RGB565 each, 4 x 108,578 B = 434,312 B, about 6 % of the 6,656 KiB NOS budget (`face_segment.c:37`, `:145`; `core/psram.h:14`). Also the scaled digit masks, `fk_sfont_build_step` (`face_segment.c:328`, `nos/faces/faces.c` glyph builder). The cache is never freed after warm-up (no free call outside the failure path).
- **Navigation:** a tap calls `nos_open(&scr_activity, x, y)` (`nos/apps/sys_home.c:1126-1127`), which pushes the Activity screen (`nos/apps/registry.c:45-50`).

## Background behaviour

The face and the services that feed it run on different schedules.

The face itself:
- `tick` and `draw` run only from the home pager (`nos/apps/sys_home.c:625`, `:901`, `:953`). `sys_home.c:623-625` skips the tick while another app covers home. The picker draws it with `sys_home_ctx()` (`nos/apps/sys_faces.c:136-137`).
- No task, timer or ISR belongs to the face. `nos/faces/` has no `xTask*` or timer call.
- Screen off (`PWR_OFF`): the face does not tick or draw.
- AOD: `nos_frame_run` runs once per second (`firmware/main/nos_main.c:2114-2116`), so the AOD face ticks and draws at 1 Hz.
- Awake: once per frame (`nos/nos.c:2299` `frame_run`, `:2341` services and screens).

The services that feed it:

| Runner | Where | Period / trigger | What it does for SEGMENT |
|---|---|---|---|
| UI loop | `app_main`, `firmware/main/nos_main.c:1899`; no task priority or core set in this file | Per frame when on (`nos/nos.c:2341`); once per second when screen off (`nos_main.c:2091-2094`); once per second in AOD (`nos_main.c:2114-2116`) | Runs `services_tick` → `svc_tick` (`nos/apps/registry.c:62-71`) |
| `svc_tick` 1 Hz gate | `nos/svc.c:1303`, gate at `:1315` | 1 s | Calls `sys_tick` (battery and charging, `svc.c:987-995`) and `act_tick` (steps, goal, kcal, minutes, `svc.c:529-558`) |
| `board-sense` task | `board_task`, `firmware/main/board.c:967`; `xTaskCreateStaticPinnedToCore` at `board.c:1036`, priority 2, `tskNO_AFFINITY`, 4 KB stack | About 20 ms cycle (`board.c:1023-1025`) | Reads the IMU, updates the pedometer, publishes `s_activity` (`board.c:985-1010`). Keeps running with the screen off or AOD. Stops only on the shutdown and sleep path (`board.c:1126-1138`, called from `nos_main.c:797` and `main.c:521,535`). |
| `board-fs` task | `board_fs_task`, `firmware/main/board.c:919`; `xTaskCreateWithCaps` at `board.c:1042`, priority 1, 4 KB, internal RAM | 100 ms loop; pedometer checkpoint every 60 s, 5 s retry on failure (`board.c:954-955`) | Saves the pedometer snapshot to NVS key `pedometer` (`board.c:915`) while background persistence is allowed (`board.c:46-47`). Allowed in OFF and AOD (`nos_main.c:2098`, `:2110`), off when awake (`nos_main.c:2141`). |
| `act_save` | `nos/svc.c:496-508`, called from `act_tick` at `svc.c:558` | When dirty: every 10 min, or 5 s after a failed write | Writes `nosact` (see Persistence). Not gated by background persistence. |

Net effect: with the screen off, steps keep counting (board-sense), the activity totals keep updating at 1 Hz (`svc_tick`), and the pedometer checkpoint keeps running. The face itself draws nothing until the screen is on or in AOD.

## Persistence

- The face persists nothing. On each boot the rings start from zero: `s_shown` and `s_from` are cleared while `s_cold` (`face_segment.c:274-278`). The last shown fill is kept in RAM only, so a build-in after leaving and returning to the face animates from what was on screen (`face_segment.c:241`, `:237-243`).
- The PSRAM cache is built once after show and kept until reboot (`face_segment.c:323-331`, `:219-232`).
- Behind the face, the data is stored as follows:
  - Activity history: key `nosact`, struct `act_saved` (`nos/svc.c:427-434`, version 2, `:435`). Holds the day key, the previous six days, hourly bins, active minutes and the day's max steps. Saved with `hal_store_save_checked` (`svc.c:499`). On this board that goes to NVS, not SD: `firmware/main/hal_esp32.c:234` sends every key except `nosprefs` and `wifi_cred` to NVS. Saved when dirty every 10 min, with a 5 s retry after failure (`svc.c:558`).
  - Pedometer snapshot: key `pedometer` in NVS (`firmware/main/board.c:915`, every 60 s while background persistence is allowed, `board.c:954-955`).
  - Step goal and 12/24 h format: the `nosprefs` record (`nos/prefs.c:155`, `:147`). It goes to SD under `/sdcard/watch` with an NVS mirror (`firmware/main/hal_esp32.c:228-250`).
- Nothing this face shows is stored under a key of its own.

## Navigation

- **Opened from:** the home pager's page 0 when `nos_prefs.face` = 5 (`nos/apps/sys_home.c:273-275`, `:435`; table order `nos/faces/faces.c:15-16`). The face picker writes the index (`nos/apps/sys_faces.c:373-374`). The serial bench command `face N` sets `nos_prefs.face` and restarts the build-in, without a prefs save (`firmware/main/nos_main.c:1388-1390`).
- **Opens:** `scr_activity`, the Activity app (`nos/apps/fit_activity.c:547`), on a tap outside radius 160 or in the values band (`face_segment.c:603-621`). The iris starts at the tap point (`sys_home.c:1127`). The REC chip takes precedence over the face tap (`sys_home.c:1062`, `:1119-1124`).
- **Home-level gestures on the face (not SEGMENT code):** a long press opens the face picker `scr_faces_home` (`sys_home.c:1135-1140`). A vertical drag on page 0 pulls Control Center (drag down, `sys_home.c:1090`) or Notifications (drag up, `sys_home.c:1091`) when `|H.x| < 40`.
- **Dead zone:** taps inside radius 160 but outside the values band (the time, the date, the unread dot at y 98) return no screen (`face_segment.c:614-618`, `:621`). The tap is consumed and nothing happens.
- Notifications are reached by a drag from the face, not by tapping the unread dot.

## Gaps

1. **No-PSRAM fallback unreachable; rings vanish.** `face_segment.c:512` returns before the dot fallback at `:533-539` while `s_cold` is set. `s_cold` clears only in `cold_track` after the cache is up (`face_segment.c:228-231`, `:330`). If `cache_init` cannot get PSRAM (`face_segment.c:138-147`), no ring draws while the allocation keeps failing. `face_segment_warm` retries it on every tick while `s_cold` is set (`face_segment.c:251`, `:329`). The time, values and labels still show (`face_segment.c:344-346` falls back to the ROM clock if the scaled font fails). Severity medium.
2. **Warm-up paid by other faces.** `fk_aod_update` calls `face_segment_warm()` whenever the calling face is awake and built in (`nos/faces/faces.c:61`). Eight other faces call `fk_aod_update` from their ticks (`face_dot.c:68`, `face_glyph.c:94`, `face_split.c:56`, `face_tape.c:74`, and others). So the ~0.43 MB cache and the digit masks are built (`face_segment.c:323-331`) even when SEGMENT is never the selected face. Severity medium.
3. **Calories are an estimate shown as a measurement.** The kcal value is `steps x 0.04` (`nos/svc.c:445`, `KCAL_STEP` at `:46`), with no body weight, heart rate or cadence input. The face's fallback at `face_segment.c:263` is redundant because `act_publish` always sets it. Severity medium.
4. **Kcal and minute goals are fixed.** `face_segment.c:266` hard-codes 400 kcal and 30 active minutes. Only the step goal is settable (`nos/apps/set_settings.c:1936`). Severity medium.
5. **Active minutes come from a heuristic.** A minute counts when more than 60 steps occur in one clock minute (`nos/svc.c:552-553`, comment "cadence > 60 steps/min"). It is not a sensor-measured activity time. Severity low.
6. **Heart rate absent.** `face_ctx.bpm` is set from `svc_heart()->bpm_last` (`nos/apps/sys_home.c:474`) but never read here. A glance face is expected to show the heart rate. SPLIT carries the heart tap (`nos/faces/face_split.c:225`). Severity medium.
7. **One tap target for everything.** Every ring and value tap opens the same Activity app (`face_segment.c:603-621`). There is no jump to heart, weather or the alarm. The time and date are dead to taps (`face_segment.c:614-618`). Severity low.
8. **Unread dot is presence only and not tappable.** It shows no count (`faces.c:576-580`). Its position (233, 98) is outside the values band, so a tap does nothing (`face_segment.c:598`, `:614`). Reaching notifications needs a drag from the face (`sys_home.c:1091`). Severity low.
9. **Cached invalid pedometer shows the saved day maximum.** `act_tick` publishes `s_as.max_steps` whenever `hal_activity_get` reports invalid (`nos/svc.c:533`). Invalidity covers a stale sample older than 250 ms, an unset calendar clock or prefs still loading (`firmware/main/board.c:495-505`; `:1011-1012`). The face then shows the last saved day's maximum as today's steps. Severity low.
10. **Ring beyond 200 % is invisible.** Steps stop at two laps (`face_segment.c:270-271`). The number keeps climbing, but the ring gives no further cue. Severity low.
11. **No goal-reached feedback.** The face has no haptic or banner call (`face_segment.c`, grep count 0). Nothing in `nos/` raises a goal-reached alert (grep for goal-reached, goal-met and goal-banner strings returns nothing). The Activity screen does give a tick when its ring completes (`nos/apps/fit_activity.c:182`). Severity low.
12. **AOD shows one ring only.** AOD draws only the steps dots (`face_segment.c:557-562`). Kcal and minutes are hidden, and so is the red lap (`:560-561`). Severity low.
13. **Battery visible only when low or charging.** The chip appears at 20 % or below, or while charging (`nos/faces/faces.c:582-585`). Severity low.
14. **Available data unused.** The service exposes `km`, `stand_hours`, weekly totals and hourly bins (`nos/svc.h:91-101`), and the context carries weather and the next alarm (`face.h:22-23`). SEGMENT shows none of them. Severity low.
15. **Simulator data is not on the watch.** The `SIM_DAY` fake day exists only in the simulator (`nos/svc.c:457-478`, under `#ifndef ESP_PLATFORM`). It is not a device gap, but simulator runs will look busier than the watch.

## Related

- [[Shell and faces]] (face table and tap rules), [[Face registry and shared face kit]], [[Face picker]], [[Home pager]]
- [[Health and activity]] (steps, goal, kcal and minutes sources), [[Activity]] (tap target), [[Sports and workouts]]
- [[Services hub]] (`svc_tick` and the 1 Hz services), [[Engine and scheduling]] (frame loop, AOD cadence)
- [[Storage and persistence]] (`nosact`, `pedometer`, `nosprefs`), [[Hardware and drivers]] (IMU, RTC, battery, PSRAM)
- [[Power and battery]] (battery chip, screen-off behaviour), [[Connectivity and voice]] (phone link behind the unread dot), [[Notifications]]
