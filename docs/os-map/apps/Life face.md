---
title: "Life face"
id: face-life
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Life face

## Summary

All paths are repo-relative. Everything below was read from source on 2026-10-23. Nothing was built, flashed or run on hardware, so timing and cost figures are code-derived.

- **What it is**: a watch face, not an app. Source `nos/faces/face_life.c` (252 lines). Registered as `face_life` with name `"LIFE"` (`nos/faces/face_life.c:252`), entry 4 of `nos_faces[]` (`nos/faces/faces.c:15-16`). Chosen through the face picker and stored as an index in `nos_prefs.face` (see Persistence). Not the default: the default is index 0, DOT (`nos/prefs.c:148`).
- **What it shows**: the hour as a 10 x 6 grid of minute dots, the day as a ring of 24 hour dots, the date, the time in small digits, the battery only when low or charging, and an unread dot.
- **Self-contained code**: `face_life.c` makes no direct `svc_*`, `prefs_*`, `hal_*`, haptic or keep-awake call (grep-verified). All data arrives through the home context (`face_ctx`, `nos/faces/face.h:7-24`), filled by `build_ctx()` in `nos/apps/sys_home.c:465-488`, and through the shared helpers in `nos/faces/faces.c` and `nos/faces/face_kit.h`.
- **Inputs it actually uses**: local time, battery percent, charging flag, unread count, the engine clock, AOD state, build-in progress, recording chip and picker dimming. Steps, heart rate, weather and next alarm are populated into the context but never read by this face.
- **Hardware**: the display for drawing. Time comes from the ESP-IDF system clock, seeded from the PCF85063 RTC on B (`firmware/main/board.c:862-885`, `firmware/main/pins.h:54,62`). Battery comes from the ADC on GPIO4 with divider 3 (`firmware/main/pins.h:65-66`). No sensor, radio or storage is needed. The IMU matters only for the raise-to-wake that brings the face back.
- **Status**: working. Every drawn element is wired to live data. The gaps are about what the face leaves out, and two data-trust problems: the B "charging" flag and an unset clock.
- **Links**: [[Shell and faces]], [[Face picker]], [[Home pager]], [[Hardware and drivers]], [[Power and battery]], [[Engine and scheduling]].

## Features

Awake, face on the glass:

- **Minute grid.** 10 columns x 6 rows at 32 px pitch, 23 px dots (`face_life.c:24-29`). Each row is a ten-minute block and each column a minute, so 10:42 shows four full rows and two dots (`face_life.c:3-4`, `:207`). Past minutes are `C_PRI` (#EEEEEE). Future minutes are `C_FUT` = `C_SURF3` (#2A2A2A) (`face_life.c:31`, `:147-150`; `nos/ui.h:26`).
- **Current minute.** A red dot (`fk_red`, which turns white while recording, `nos/faces/face_kit.h:151-155`). It breathes on a 2 s cycle: a symmetric triangle through a smoothstep, quantised to 8 steps, sizing the dot between about 0.78 and 1.0 of 23 px (`face_life.c:237-245`).
- **Previous minute whitens** over 3 frames on a minute change, and the new dot snaps up from 40% (`face_life.c:70-73`, `:145`, `:244`).
- **Changed digits roll** on a minute change. Digits sit in tabular cells centred on the width of "0" (`face_life.c:178-195`).
- **Day ring.** 24 hour dots on radius 212 (`face_life.c:30`, `:162-176`). Past hours `C_SEC` (#9C9C9C). The current hour is `C_WHITE` at 11 px. Every 6th hour is 9 px, others 6 px. Future hours are `C_LINE2` (#333333) (`nos/ui.h:28`).
- **Time.** `fk_time` gives "HH:MM" in 24 h, or "H:MM" in 12 h with no leading zero (`nos/faces/faces.c:85-99`). The default is 24 h (`nos/prefs.c:147`).
- **Date.** "SUN 11" from `fk_date` with fixed English three-letter weekday names (`nos/faces/faces.c:107-110`; `nos/ui.c:914-918`). It crossfades at midnight (`nos/faces/face_kit.h:60-61`; `face_life.c:95`, `:233`).
- **Status line** at y 408 (`face_life.c:34`, `:235`). Shows "NN%" with a bolt while charging, or "NN%" alone at or below 20% (`nos/faces/faces.c:582-600`).
- **Unread dot** at (UI_CX, 64), 8 px, `C_PRI` (`face_life.c:38`, `:249`; `nos/faces/faces.c:565-580`). It is only a dot; the count is not shown. It fades in and out in four ink steps (`nos/faces/faces.c:552-563`).
- **Recording.** The REC chip shares the status line at y 408 (`nos/apps/sys_home.c:191`). The red dot becomes a white ring (`face_life.c:153-160`).

Motion (the header comment and the implementation agree on these):

- **Build-in on entry**: the grid lights in reading order, and the day ring fills with it. Duration is `ENTER_S` 0.40 s, 12 frames (`nos/apps/sys_home.c:55`; `nos/faces/face_kit.h:31`; `face_life.c:46-63`).
- **Top of the hour**: the grid drains at 6 dots a frame, then the new red dot snaps up (`face_life.c:35`, `:66-69`).
- **Wake from AOD**: the AOD dots grow into the grid over `WAKE_N` = 6 frames (`nos/faces/faces.c:455`; `nos/faces/face_kit.h:95-101`).

Always-on (AOD) variant (`face_life.c:203-223`):

- Grid dots are grey: past 7 px `C_DIS`, future 3 px `C_LINE2` (`face_life.c:135-141`). The current minute is a ring of 10 to 14 px in `C_SEC` (`face_life.c:210-213`).
- The day ring is hidden (`face_life.c:168`).
- The time is drawn in `C_SEC` (`face_life.c:216`).
- The date is hidden. The code shows it only when `k > 0` (`face_life.c:215`), and `fk_wake_k` returns 0 in AOD (`nos/faces/faces.c:482-484`).
- The status shows only below 10% and not charging (`nos/faces/faces.c:582-584`; `face_life.c:218-220`).
- Burn-in drift of up to 4 px, stepped once a minute (`nos/faces/faces.c:19-26`, `:57-63`).

Gestures that land on this face (shared home shell, not Life code):

- Long press opens the face picker (`nos/apps/sys_home.c:1135-1140`).
- A vertical drag from the face pulls the control centre (down) or notifications (up) (`nos/apps/sys_home.c:1090-1091`).
- A tap on the face does nothing by itself (see Navigation).

## Reads

Home context (`face_ctx`, `nos/faces/face.h:7-24`), filled in `build_ctx()` (`nos/apps/sys_home.c:465-488`):

| Field | Filled from | Underlying source | Used by Life |
|---|---|---|---|
| `dt` (local date, time, weekday, millis) | `hal_get_datetime()` (`sys_home.c:468`) | `gettimeofday` + `localtime_r` (`firmware/main/hal_esp32.c:116-127`, `:142-146`) | yes: minute, hour, date, breath phase |
| `battery` | `svc_sys()->battery_pct` (`sys_home.c:474`), else `hal_battery_percent()` (`firmware/main/hal_esp32.c:392`) | `sys_tick` at 1 Hz (`nos/svc.c:991`, `:1315-1317`), fed by `hal_esp32_sample_power` at 1 Hz (`firmware/main/board.c:1021`) | yes: status line |
| `charging` | `svc_sys()->charging` (`sys_home.c:475`) | `nos/svc.c:992`: `external_power == 1` (see Gaps 1) | yes: status line |
| `unread` | `svc_notif_unread()` (`sys_home.c:476`) | `nos/svc.c:238-242`: counts `!read` in `B->n[]` | yes: unread dot |
| `t` | engine clock `nos_t` (`sys_home.c:483`) | engine | yes: all animation |
| `aod` | `nos_aod && !aod_dimming_out()` (`sys_home.c:484`) | power state (`firmware/main/nos_main.c:705`) | yes |
| `enter` | `(nos_t - H.enter_t0) / ENTER_S` (`sys_home.c:485`) | home | yes: build-in |
| `rec` | `H.rec_raw` quantised (`sys_home.c:488`) | REC chip | yes, through `fk_red` / `fk_rec` |
| `lite` | picker dimming (`nos/apps/sys_faces.c:146-151`) | face picker | yes, through `fk_ghost` |
| `steps`, `step_goal`, `bpm`, `weather_valid`, `temp_c`, `wx_cond`, `next_alarm_min` | `sys_home.c:471-482` | activity, heart, weather and alarm services | **no**: populated, never read |

Other reads:

- `nos_prefs.hour24` through `fk_hour_disp` (`nos/faces/faces.c:79-83`).
- `hal_millis()` inside `fk_wake_tick` (`nos/faces/faces.c:459`).
- `ui_weekday3()` for the day name (`nos/ui.c:914-918`).
- Sim-only probes (`face_life.c:96-118`, `nos/faces/faces.c:508-550`) are compiled out of the device build.

Not read: no `svc_*` getter of its own, no `prefs_*` key, no NVS, no SD file, no `hal_hr_*` and no direct `hal_power_get`. The face reads no sensor.

Hardware read through the above:

- RTC PCF85063 at 0x51 on B (`firmware/main/pins.h:54`, `:62`), restored at boot (`firmware/main/board.c:862-885`).
- Battery ADC on GPIO4, system rail divided by 3 (`firmware/main/pins.h:65-66`). Percent is a LiPo curve over the smoothed rail voltage (`firmware/main/hal_esp32.c:316-362`).
- USB-serial host detection drives `charging` on B (`firmware/main/hal_esp32.c:323`, `:337`). See Gaps 1.

## Writes

The face writes no persistent state and no hardware state.

- **Own RAM state** `S` (`face_life.c:40`): `change_t`, `last_m`, `tfrom` / `tto` (the digit strings), the `fk_wake` morph, and the `fk_xfade` date crossfade. `enter()` resets only `last_m` (`face_life.c:42`). `S` survives leaving the face and is cleared only by a reboot.
- **Shared kit RAM** (not owned by Life, but changed by its calls): `fk_wake` state, the unread fade `s_unread_k`, the second tracker `s_sk`, the epoch counter `s_dc_epoch` (`nos/faces/faces.c:35`, `:41-47`, `:57-63`), and `fk_aod_dx` / `fk_aod_dy`.
- **PSRAM dot cache** `s_dc`, allocated once with `hal_big_alloc` (`nos/faces/faces.c:240-246`). It is invalidated every frame (`nos/faces/faces.c:248-249`).
- **Framebuffer only**: damage commands and dot discs (`nos/faces/faces.c:300-330`; `face_life.c` `dot_disc` and `ui_text` calls).
- Nothing is written to `nos_prefs`, `svc_*`, NVS, SD, haptics or the sensor lease. Life never calls `nos_keep_awake()`, so it does not hold the screen on.
- **The face choice** is written by the picker, not by Life: `nos_prefs.face = p; prefs_save();` (`nos/apps/sys_faces.c:373-375`).

## Background behaviour

Who runs what, and when. Periods are the code's values.

| Runner | Where | Period | When it runs |
|---|---|---|---|
| Home tick, which calls `f->tick` (Life `tick`) | `nos/apps/sys_home.c:514`; face tick at `:625` | Every awake frame. Default cap 60 fps (`nos/frame_cap.h:5`, `nos/prefs.c:161`), or the `fps_cap` pref (`firmware/main/nos_main.c:2132-2135`) | Only when home is the top screen and the face is not hidden by an app page (`sys_home.c:621-625`) |
| Screen tick dispatch | `nos/nos.c:2101-2109` | Per frame | Only the top screen ticks. During a transition, `under` and `over` tick (`nos.c:2102-2105`). Life's tick therefore stops under any other screen |
| AOD frame | `firmware/main/nos_main.c:2106-2126` (`nos_frame_run` at `:2116`) | 1 Hz (`now - last_aod >= 1000000`) | PWR_AOD or PWR_SENSOR. Home ticks and draws the AOD face once a second. PWR_SENSOR also drops the CPU to 80 MHz |
| Idle target | `nos/power_policy.h:13-14`; `firmware/main/nos_main.c:2084-2085` | After `sleep_s` with no input (default 15 s, `nos/prefs.c:149`) | Default `sensor_idle` = 1 (`nos/prefs.c:159`) sends the watch to PWR_SENSOR. So with defaults the idle screen is the AOD Life face, at 1 Hz. PWR_OFF happens only on low battery (`power_idle_target`), or when both `sensor_idle` and `aod` are off |
| Screen off (PWR_OFF) | `firmware/main/nos_main.c:2091-2094` | 1 Hz | Only `nos_bg_tick`, which is `services_tick` = `svc_tick` + `background_tick` (`nos/apps/registry.c:62-65`). No screen tick, no draw |
| Service tick | `nos/svc.c:1303-1320` (called from the frame loop at `nos/nos.c:2341` and from the screen-off path at `nos_main.c:2094`) | Per call | `sys_tick()` at 1 Hz: battery, charging, steps (`svc.c:1315-1317`, `:987-992`). `companion_tick()` drains notification posts on each call (`svc.c:1256-1259`, `:1313`) |
| Battery sampler | `firmware/main/board.c:1021` | 1 Hz | Always, including screen off. Reads the ADC (`firmware/main/hal_esp32.c:316`) |
| Clock | ESP-IDF system clock | Continuous | Always. Seeded from the RTC at boot (`firmware/main/board.c:862-885`). Set over USB, BLE or SNTP |

Behaviour with the face closed or the screen off:

- Life does no work while closed. Its per-minute state advances only inside the home tick. When the face returns after a gap of more than 0.5 s, `fk_wake_stale` makes the next tick settle the morphs rather than replay a change nobody saw (`nos/faces/faces.c:457-461`; `face_life.c:84`, `:87`, `:91`). The time is then recomputed from the clock.
- Notifications keep arriving through `svc_tick`, so the unread count can rise while the face is off. The dot appears on the next home frame that runs.

Per-frame cost: `face_life.c:17-18` says a frame costs only the red dot's box. This has not been measured here. The dot list cache is invalidated every frame (`nos/faces/faces.c:248-249`), so any saving depends on the chunk-hash damage path (`nos/faces/faces.c:300-330`). It needs a bench measurement.

## Persistence

- **The face itself**: nothing. No prefs keys, no NVS, no SD file. All animation state is in RAM and resets on reboot.
- **Chosen face**: `nos_prefs.face`, a `uint8` index, default 0 (`nos/prefs.c:148`). Written by `prefs_save()` when the picker finishes (`nos/apps/sys_faces.c:370-375`). `prefs_save` has a 2 s debounce and a 10 s maximum dirty age (`nos/prefs.h:55`). On B the prefs go to SD under the watch directory as `settings.N.bin` (`nos/prefs_disk.c:32`; `CLAUDE.md`), with a legacy NVS migration (`nos/prefs.c:257`). The stored value is an index, not a name (Gaps 11).
- **Time**: not stored by the face. The RTC keeps time on B (`firmware/main/pins.h:62`) and is restored at boot (`firmware/main/board.c:862-885`).
- **Unread count**: RAM only. Notifications sit in a PSRAM buffer (`nos/svc.c:58`, `:1281`) with no NVS or SD write. The count resets at reboot. Entering the notifications sheet marks everything read (`nos/apps/sys_notifs.c:276`, `:292`).
- **Battery calibration** key `bat_cal` (`firmware/main/hal_esp32.c:318`) is read by the sampler, not by the face.

## Navigation

- **Opened from**:
  - The home pager, face page 0, when `nos_prefs.face` is 4 (`nos/faces/faces.c:15-16`; `nos/apps/sys_home.c:1126`).
  - The face picker, opened by a long press on the home face (`nos/apps/sys_home.c:1135-1140`). The picker builds the face in place and saves the choice on finish (`nos/apps/sys_faces.c:370-380`).
- **Opens**: nothing. The face's `tap` is null (`face_life.c:252`), and home's tap routing yields no screen for a null tap (`nos/apps/sys_home.c:1126`). A tap on the time, date, battery or unread dot does nothing.
- **Other taps on the face**: the REC chip, when recording. It lights for 3 frames and then opens the recorder (`nos/apps/sys_home.c:1119-1124`). Widget pages 2 and up are reached by horizontal paging, with taps routed to the widget (`nos/apps/sys_home.c:1127-1129`).
- **Vertical drag from the face**: down pulls the control centre, up pulls notifications (`nos/apps/sys_home.c:1090-1091`).
- **Leaves**: a horizontal swipe to a widget page, or a sheet, app or picker covers it. Returning from a short cover skips the build-in if the face never left the glass (`nos/faces/faces.c:456`, the `fk_wake_skip` flag).

## Gaps

Ordered by severity. Evidence is repo-relative path:line.

1. **[medium] The "charging" flag on B is USB host attachment, not charge detection.** `usb = usb_serial_jtag_is_connected()` (`firmware/main/hal_esp32.c:323`, with the comment "SOF from a host, not charge current"). The charge-status pin is used only if `PIN_CHG_STAT` is defined (`hal_esp32.c:324-325`). It is defined only in the 1.43C block (`firmware/main/pins.h:33`, inside the `#if CONFIG_WATCH_BOARD_143C` block at `:7-35`). So B has no charge input, which matches the CLAUDE.md table. Then `external_power = usb ? 1 : -1` (`hal_esp32.c:337`), and `charging = external_power == 1` (`nos/svc.c:992`). Impact: the bolt and "NN%" appear when a PC is attached over USB, even if the cell is not charging. A plain charger with no USB host reads as not charging, and no bolt appears. The face gives the user no way to tell these apart.

2. **[medium] An unset clock shows a 1970 date and time, with no "set time" hint.** When the RTC has no valid time, the system clock is set to 0 (`firmware/main/board.c:882-886`). `hal_get_datetime` does not validate (`firmware/main/hal_esp32.c:142-146`). `build_ctx` does not check either (`nos/apps/sys_home.c:468`). The only UI checks for an unset clock are in `nos/apps/fit_workout.c:830` and `nos/apps/set_settings.c:2370`. Impact: Life shows a 00:00-ish time and an epoch date, with no indication that the time is unset.

3. **[medium] Steps, heart rate, weather and next alarm are fed to the face but never shown.** `build_ctx` fills them every frame (`nos/apps/sys_home.c:471-482`). Nothing in Life or the kit reads them. Impact: the face has no glanceable complication. A modern watch face usually shows at least steps, the next alarm or the weather. Life is a pure clock.

4. **[medium] Unread is a dot with no count, and it resets at reboot.** Life draws only the dot (`face_life.c:249`; `nos/faces/faces.c:565-580`). The count lives in a PSRAM buffer with no NVS or SD write (`nos/svc.c:58`, `:238-242`, `:1281`). After a reboot the dot stays off until new notifications arrive. Entering the notifications sheet clears all of them at once (`nos/apps/sys_notifs.c:276`, `:292`).

5. **[low] Taps on the face do nothing.** `face_life` sets `tap` to 0 (`face_life.c:252`), and home's tap routing yields no screen for it (`nos/apps/sys_home.c:1126`). The time, date, battery and unread dot are dead targets. The alarm, timer and notification screens are reachable only by gesture.

6. **[low] A placeholder battery percent can show as real at boot on USB.** `hal_power p = s_power` (`firmware/main/hal_esp32.c:319`), and `s_power` starts at `battery_percent = 50` (`hal_esp32.c:287`). The percent changes only after a sample in the 3300-4300 mV window (`hal_esp32.c:350-361`). `external_power` is set to 1 from the USB check alone (`hal_esp32.c:337`). So with a USB host attached and no valid ADC sample yet, the face can show "50%" with the bolt. Separately, `hal_power_get` marks a stale sample invalid after 3 s but does not clear `battery_percent` (`hal_esp32.c:378-386`), so the last value persists.

7. **[low] The battery is shown only at 20% or below, or while "charging."** `fk_status_shown` (`nos/faces/faces.c:582-584`). In AOD it shows only below 10% (`faces.c:582-584`). There is no normal battery level on this face.

8. **[low] Seconds are not shown.** The only sub-minute signal is the 2 s breath of the red dot (`face_life.c:237-244`).

9. **[low] The date is English only.** `ui_weekday3` has fixed English names (`nos/ui.c:914-918`), and `fk_date` uses them (`nos/faces/faces.c:107-110`). There is no locale support.

10. **[low] The date is hidden in AOD.** `face_life.c:215` shows the date only when `k > 0`, and `fk_wake_k` returns 0 in AOD (`nos/faces/faces.c:482-484`). This may be intended, but nothing documents it.

11. **[low] The saved face is an index.** `nos_prefs.face` stores a position in `nos_faces[]` (`nos/apps/sys_faces.c:373-375`; `nos/faces/faces.c:15-16`). Reordering or inserting a face silently changes what a saved index selects. There is no migration.

12. **[low] The 12 h minute roll is skipped at 9:59 to 10:00 and 12:59 to 1:00.** The digit count changes, so `face_life.c:93` copies the new string over the old and the digits snap instead of rolling. Cosmetic.

13. **[low] The per-frame cost claim is unverified.** `face_life.c:17-18` says only the red dot's box costs per frame. The dot cache is rebuilt each frame (`nos/faces/faces.c:248-249`), and `grid_dot` and `day_dot` run for every dot each frame (`face_life.c:205-206`, `:230-231`). Whether that is cheap depends on the chunk-hash damage path (`nos/faces/faces.c:300-330`). Needs a bench measurement.

Doc versus code:

- `face_life.c:6-7` says the battery shows "when low / charging". The code does that (`faces.c:582-584`), but "charging" on B means USB host attachment (Gap 1).
- `face_life.c:18` says "12 h: 9:41". That describes an option. The default is 24 h (`nos/prefs.c:147`), so the face shows "09:41".
- `nos/faces/faces.c:455` says the wake morph is "0.2 s at 30 fps". The default cap is 60 fps (`nos/frame_cap.h:5`), so the morph is about 0.1 s at default. The comment is stale.
- `nos/faces/face_kit.h:124-126` lists LIFE among faces that always draw the unread dot in `C_PRI`. The code matches (`face_life.c:249`).
- `nos/apps/sys_home.c:184-187` says LIFE keeps the battery on the status line, with the REC chip taking the line first. The code matches (`sys_home.c:191`).

Related notes:

- [[Services hub]]: `svc_sys()`, `svc_notif_unread()`, `svc_tick` (Gaps 1, 2, 4).
- [[Power and battery]]: the battery sampler, the idle target, PWR_SENSOR and PWR_OFF (Gaps 6, 7).
- [[Storage and persistence]]: prefs on SD, RAM-only notifications and face state (Persistence, Gap 11).
- [[Hardware and drivers]]: the RTC, the battery ADC and USB detection on B (Gaps 1, 2, 6).
- [[Connectivity and voice]]: the notification source feeding the unread count (Gap 4).
- [[Engine and scheduling]]: the top-screen tick, the 1 Hz AOD frame and the 1 Hz screen-off service tick.
- [[Health and activity]]: steps and heart rate are in the context but not drawn (Gap 3).
- [[Sports and workouts]]: not used by this face.
