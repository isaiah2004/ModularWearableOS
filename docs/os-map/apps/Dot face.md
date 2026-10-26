---
title: "Dot face"
id: face-dot
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Dot face

Source: `nos/faces/face_dot.c` (156 lines). Shared helpers: `nos/faces/faces.c`, `nos/faces/face_kit.h`, `nos/dot.c`. Home glue: `nos/apps/sys_home.c`. Engine and power loop: `nos/nos.c`, `firmware/main/nos_main.c`.

Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Board facts are for the 1.43-B build (`firmware/main/pins.h:36-66`).

## Summary

DOT is the default watch face (`nos/prefs.c:148` sets `face = 0`; `nos/faces/faces.c:15-16` lists `face_dot` first). It draws HH over MM in the 5x9 dot font on a 17 px lattice (`face_dot.c:21-26`), one red seconds dot between them that blinks once a second (`face_dot.c:150-151`), the date ("SUN 11") above, a battery line that appears only at 20 % or below or while charging (`faces.c:582-600`), and a red unread dot at the top (`faces.c:565-575`).

The face owns no data and keeps no persistent state. Its inputs come from the home context builder (`sys_home.c:465-490`), which fills the clock, the battery and charge fields from `svc_sys()`, and the unread count from `svc_notif_unread()`. The face itself reads only the clock, `battery`, `charging`, `unread`, the AOD flag, the build-in progress, the picker dim level and the REC level. It has no tap hook (`face_dot.c:155`, last field `0`), so a tap on it opens nothing.

Status is working: every feature in the file is reached on the 1.43-B path. The gaps are what the face leaves out (no complications, no 12-hour AM/PM, no "clock not set" state) and what its battery and charge signals mean on B. On B, "charging" is a USB-host flag, not charge current (`firmware/main/hal_esp32.c:323`).

## Features

- **Time.** HH and MM in `DOT_FONT_TALL` on one lattice, pitch 17 px, lit dot 0.8 pitch (`face_dot.c:21-22`, `:146-148`). Unlit cells show as ghost dots in `#181818` (`face_dot.c:30`, `:139`).
- **24 h or 12 h.** `nos_prefs.hour24` (default 1, `prefs.c:147`) selects the mode through `fk_hour_disp` and `fk_hh` (`faces.c:79-90`). In 12 h the leading zero becomes a blank, and a lone hour digit is centred (`face_dot.c:62-63`). No AM/PM mark is drawn.
- **Seconds.** One red dot at the centre (`face_dot.c:150-151`). Its envelope is 4 steps up, a 500 ms hold, then 3 steps down (`faces.c:801-810`). During the build-in it snaps in over the last frames (`face_dot.c:41-45`). It turns white while the REC chip is up (`fk_red`, `face_kit.h`).
- **Date.** Weekday and day only, for example "SUN 11" (`faces.c:107-110`; English names from `nos/ui.c:914-917`). On change it cross-fades over 5 frames (`faces.c:146-168`; `face_dot.c:66-67`, `:86`).
- **Battery line.** At `Y_STAT` 428 (`face_dot.c:28`, `:136`). Shows "NN%" at 20 % or below, or while charging, with a bolt when charging (`faces.c:587-600`).
- **Unread dot.** 8 px red at (233, 24), fading in four ink steps. There is no count (`faces.c:552-575`; `face_dot.c:152`).
- **Build-in** on show and on wake, `ENTER_S` 0.40 s (`sys_home.c:55`, `:278-283`). HH sparkles in over frames 0-7 and MM over frames 2-9, the date fades up in six steps, the seconds dot snaps (`face_dot.c:8-12`, `:140-148`).
- **Minute change.** An 8-step morph at 40 Hz, 0.2 s (`face_dot.c:29`, `:94-112`; `faces.c:134-136`). When the hour gains or loses a digit in 12 h, the shared slot steps in whole pitches.
- **AOD.** Thin grey dots (`D_AOD`, `face_dot.c:23`, `:120-133`), no seconds, and a 6 px grey unread dot (`faces.c:568`). The whole face drifts within plus or minus 4 px, stepping once a minute (`faces.c:20-26`). A battery below 10 % and not charging shows as a bare "N%" (`faces.c:582-585`).
- **Wake from AOD.** The grey dots grow into the awake style over 6 ticks, and the drift walks home 1 px a tick (`faces.c:455`, `:458-476`, `:498-506`).
- **Picker.** When the picker dims or slides the face, the ghost layer fades out (`fk_ghost`, `face_kit.h`; `nos/apps/sys_faces.c:146-151`).

## Reads

Services (svc):
- `svc_sys()` for `battery_pct` and `charging` (`sys_home.c:474-475`; `nos/svc.c:986-992`). `sys_tick()` refreshes both once a second (`svc.c:987-995`, `:1315-1317`).
- `svc_notif_unread()` for the count of unread entries (`sys_home.c:476`; `svc.c:238-243`).
- Filled for every face but not read by DOT: `svc_activity()` (steps, goal), `svc_heart()`, `svc_weather()` and `svc_alarm_next()` (`sys_home.c:477-482`). DOT ignores `steps`, `step_goal`, `bpm`, `weather_valid`, `temp_c`, `wx_cond` and `next_alarm_min` (`nos/faces/face.h:12-27`).

Prefs (`nos_prefs`):
- `hour24` only (`faces.c:81`, `:89`). Default 1 (`prefs.c:147`).
- Not read by DOT: `face_accent` (no reader anywhere in `nos/`, `prefs.h:21`), `theme` (applied at panel level, `firmware/main/nos_main.c:379-385`), and `aod` (read only by the power state machine, `firmware/main/nos_main.c:2085`).

HAL:
- `hal_get_datetime()` fills `c->dt` on every home tick (`sys_home.c:468`; `firmware/main/hal_esp32.c:142-146`). Local time comes from `gettimeofday` plus `localtime_r` (`hal_esp32.c:116-130`).
- `hal_battery_percent()`, only when `battery_pct` is 0 (`sys_home.c:474`; `hal_esp32.c:392`). Both read the same snapshot, so the fallback adds nothing.
- `hal_millis()` in the wake tracker (`faces.c:460`).
- `hal_big_alloc()` (PSRAM) for the dot glyph tables and mask cache (`nos/dot.c:83-88`, `:249-258`).

Engine state (neither svc, prefs nor hal): the animation clock `nos_t`, `nos_aod` (`sys_home.c:484`), the build-in progress from `H.enter_t0` (`sys_home.c:483-486`), `H.rec_raw` (`sys_home.c:488`), and the picker `lite` level (`sys_faces.c:146-151`).

## Writes

- Only RAM and the framebuffer. The face's own state is in `static S` (`face_dot.c:32-36`): two morph trackers, one wake tracker and the date cross-fade. `enter()` resets the morph trackers (`face_dot.c:38`).
- Shared statics in `faces.c`: the second-clock origin (`faces.c:40-55`), the unread fade level `s_unread_k` (`faces.c:552-559`) and the AOD drift `fk_aod_dx/dy` (`faces.c:34`, `:57-63`).
- A cross-face side effect. While DOT is awake and built in, `fk_aod_update` calls `face_segment_warm()` (`faces.c:61`). That warms the SEGMENT face's font and glyph cache one slice a frame (`nos/faces/face_segment.c:323-331`).
- No prefs, svc, hal or storage writes. The unread count is cleared by the notification list, not by DOT (`nos/apps/sys_notifs.c:292`).
- Bench only: the serial command `face N` sets `nos_prefs.face` and rebuilds the face, with no `prefs_save()` in that branch (`firmware/main/nos_main.c:1388-1390`).

## Background behaviour

DOT has no task of its own. Its tick and draw run inside the UI frame loop.

- **Loop.** `app_main` runs the frame loop (`firmware/main/nos_main.c:1899`, loop at `:1977`). `fps_cap` defaults to 60 (`prefs.c:161`; `nos/frame_cap.h:5`), so the loop is paced at 60 Hz (`nos_main.c:2135-2136`; `nos/render_config.h:13`). Motion is clock-based: `fk_frames` counts at `M_FPS` = 40 (`faces.c:70-77`; `nos/motion.h:25`).
- **Home on top.** `home_tick` calls `f->tick` (`sys_home.c:625`). Draw runs once per 16-row stripe (`faces.c:205-207`).
- **App on top.** When an app is on top, `tick_screens` ticks only that screen (`nos/nos.c:2101-2109`), so DOT's tick stops. On return, a gap of more than 0.5 s makes `fk_wake_stale` settle the morphs instead of replaying them (`faces.c:457-462`).
- **Awake, idle.** After `sleep_s` (default 15, `prefs.c:149`) without touch, the loop calls `power_idle_target` (`nos_main.c:2084-2085`; `nos/power_policy.h:13-14`). With the default `sensor_idle` = 1 (`prefs.c:159`), the target is `PWR_SENSOR`: not AOD and not off. `go_state` then sets `nos_aod = 1` (`nos_main.c:705`). The AOD branch runs DOT at 1 Hz, sets the CPU to 80 MHz (`nos_main.c:2108`), and keeps the panel lit at AOD brightness (`nos_main.c:2114-2122`). Under default settings, DOT's AOD variant is therefore the screen-off display, redrawn once a second.
- **Screen off (`PWR_OFF`).** No face tick or draw. Only `services_tick()` runs, once a second (`nos_main.c:2091-2095`; `nos/apps/registry.c:62-66`). It refreshes the battery, charge and unread values in RAM. `PWR_OFF` is entered from AOD or SENSOR when the battery is low (`nos_main.c:2036`). The low state is entered at 15 % or below on battery and left at 20 % (`nos/power_policy.h:6-11`).
- **Battery sampling.** Task `board-sense`, priority 2, 4 KB stack, unpinned (`firmware/main/board.c:1036`), calls `hal_esp32_sample_power()` once a second (`board.c:1021`).
- **RTC.** The PCF85063 at 0x51 (`firmware/main/pins.h:54`, `:62`) restores the clock at boot (`board.c:847-875`). It is written by `rtc_save` when the clock is trusted (`board.c:806-816`).
- **One-shot at boot.** The `dotbake` task runs on core 1 at priority 1 and bakes the dot masks (`nos_main.c:1950`; `nos/dot.c:247-249`).

## Persistence

- DOT persists nothing. Every value is recomputed each frame from the clock and services.
- The face index `nos_prefs.face` is written by the picker's `finish()` through `prefs_save()` (`nos/apps/sys_faces.c:370-376`). Its default is 0 (`prefs.c:148`).
- `hour24` is a Settings row (`nos/apps/set_settings.c:534`).
- Preferences go to `/watch/settings.0.bin` and `/watch/settings.1.bin` on SD (`nos/prefs_disk.c:32`; `firmware/main/sd_xfer.c:294`), and to the NVS overlay when the card is absent (`nos/prefs.c:105`, `:128`; `firmware/main/hal_esp32.c:203-226`). See [[Storage and persistence]].
- Time persists in the RTC (see Background). The unread count is RAM only. `svc_init()` clears it at boot (`nos/svc.c:1285`), so the unread dot cannot survive a reboot.

## Navigation

- **Opened from:** home page 0, the default screen (`sys_home.c:1-4`; `cur_face()` at `sys_home.c:271-276`; face draw at `:899-902`). Home is `scr_home` (`sys_home.c:1176`). See [[Shell and faces]] and [[Home pager]].
- **Picker:** a long press on the face (`sys_home.c:1135-1141`; `home_open_faces` at `:1008-1013`), or Settings "Watch face" (`nos/apps/set_settings.c:461`, `:1852`). The picker writes the chosen index (`sys_faces.c:370-376`). See [[Face picker]].
- **Taps:** none on the face. `face_dot` has no tap hook (`face_dot.c:155`), and home calls `f->tap` only when it is set (`sys_home.c:1126-1127`). The REC chip above the face opens the recorder while it is shown (`sys_home.c:1117-1124`).
- **Leaving:** swipe to the neighbouring widget pages (`sys_home.c:1-10`), or press the side button, which goes to page 1 (the Apps list) when at the face (`sys_home.c:1154`).

## Gaps

1. **[medium] No complications and no tap targets.** Steps, heart rate, weather and next alarm reach every face (`sys_home.c:477-482`; `nos/faces/face.h:12-27`), but DOT ignores them, and it has no tap hook (`face_dot.c:155`). A modern watch face normally shows at least one of these. See [[Health and activity]] and [[Services hub]].
2. **[medium] "Clock not set" is not shown, and the face shows 1970.** With no RTC restore and no phone or USB sync, the board sets the system clock to epoch 0 (`firmware/main/board.c:882-886`). DOT reads it with no validity check (`face_dot.c:55-56`; `faces.c:107-110`). The result is 1970-01-01 at the local time of the epoch, weekday THU, day 1. The trust flag `s_system_trusted` is private to `board.c` (`board.c:68`, `:751`) and is not exposed to faces. This is inferred from code; it was not seen on a board. See [[Connectivity and voice]] for the sync paths.
3. **[medium] "Charging" on B is a USB-host flag, not charge current.** `firmware/main/hal_esp32.c:323` (comment: "SOF from a host, not charge current"), `:337`, `:350`. The `PIN_CHG_STAT` define exists only in the 1.43C branch (`firmware/main/pins.h:33`; the B branch starts at `:35`), so B has no charge-status pin. With a PC attached over USB, DOT shows the bolt and the NN% line all the time, even on battery (`faces.c:587-600`). Without a host, charge is inferred only from a rail reading of 4400 mV or more on three samples (`hal_esp32.c:347-352`). This read did not validate that heuristic. See [[Power and battery]] and [[Hardware and drivers]].
4. **[medium] The AOD toggle has no effect under default settings.** DOT's AOD variant is the default idle display because `sensor_idle` defaults to 1 (`prefs.c:159`; `nos_main.c:2085`). The Settings "Always-on display" row (`nos/apps/set_settings.c:455`, `:526`; default 0 at `prefs.c:144`) is read only when `sensor_idle` is 0 (`nos/power_policy.h:13-14`). Turning AOD off therefore does not stop the 1 Hz face. This is a code-level finding and was not measured on a board. See [[Power and battery]].
5. **[low-medium] The battery percent is a rail-voltage estimate shown as an exact integer.** `hal_esp32.c:286-289` says the rail is not a fuel gauge; the estimate is `lipo_percent` (`:300-310`, applied at `:361`). Before the first valid sample, `battery_percent` is the boot placeholder 50 (`hal_esp32.c:287`). The USB flag sets `external_power = 1` before the sample is checked (`:337`), so DOT can show "50%" with the bolt until the ADC settles. When sampling stops, `hal_power_get` clears the validity flags but keeps the last percent (`hal_esp32.c:378-388`), and DOT never reads `battery_valid`.
6. **[low] The date shows weekday and day only, in English.** No month, year or locale (`faces.c:107-110`; `nos/ui.c:914-917`).
7. **[low] No 12-hour AM/PM.** In 12 h mode the face is ambiguous (`face_dot.c:141-148`; `faces.c:85-90`). Other screens do show AM/PM (`nos/apps/time_alarm.c:760`).
8. **[low] No text fallback.** If the dot tables cannot be allocated, `text_impl` returns without drawing (`nos/dot.c:526-530`, `:83-88`). DOT then shows no digits, although the date and battery still draw. `fk_clock_fallback` exists (`faces.c:101-106`), and the matrix, segment and orrery faces call it (`face_matrix.c:332`, `face_segment.c:345`, `face_orrery.c:2193`). DOT does not.
9. **[low] `face_accent` is dead, so the red accent cannot be changed.** Nothing in `nos/` reads it (`prefs.h:21`; `prefs.c:34` is only the field list). See [[Settings and developer]].
10. **[low] The unread indicator is a dot with no count.** It is RAM only and resets at boot (`svc.c:1285`). It clears when the notification list opens (`nos/apps/sys_notifs.c:292`).
11. **[low, docs] Wake timing in the comments does not match the counter.** `faces.c:455` says "6 frames: 0.2 s at 30 fps", and `face_kit.h:96` says "about 0.2 s". The counter advances per tick (`faces.c:473-474`), so at the 60 Hz default the wake lasts about 0.10 s.
12. **[low, docs] Stale critic note.** `docs/polish/faces_r1.md:17` and `:66` say no face draws the unread dot. DOT does (`face_dot.c:152`).
13. **[unverified] Cost claim.** `face_dot.c:14-15` says idle frames repaint only the red dot's 16 px box. This read did not check it; it depends on the engine's damage tracking.

No dead buttons. No fake data beyond the boot placeholder in item 5. Values refresh at 1 Hz or on the minute, so none are stale in normal running.

## Hardware

- Panel: 466 x 466 round, the 1.43-B CO5300 path (`nos/ui.h:46-48`), drawn in 16-row stripes (`faces.c:205-207`).
- PSRAM: dot glyph tables and mask cache (`nos/dot.c:83-88`, `:248`, about 550 KB for the masks).
- Battery ADC: GPIO4, ADC1 channel 3, divider 3 on B (`firmware/main/pins.h:65-66`).
- USB serial-JTAG: the host-attached flag that stands in for charging (`hal_esp32.c:323`).
- PCF85063 RTC at 0x51, present on B (`pins.h:54`, `:62`).
- Not used directly: IMU, touch and SD. Wake from AOD comes from the engine's power states (`nos_main.c:2061`; `nos/power_policy.h:16`).
