---
title: "Glyph face"
id: face-glyph
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Glyph face

Source: `nos/faces/face_glyph.c` (253 lines). Registered as `face_glyph` (`nos/faces/face_glyph.c:253`) and listed third in the registry (`nos/faces/faces.c:15-16`), so it is `nos_faces[2]`. Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from source reading only, not from the device.

## Summary

GLYPH is the analog face of the NOS ring. It draws 60 dot ticks around the bezel, two white capsule hands (hour 122 px, minute 188 px, `nos/faces/face_glyph.c:25-28`), a red one-dot second hand on radius 202 driven by the millisecond clock, and one date label that moves between three windows. A battery line and an unread dot travel with the date. There is no digital time, no complication beyond the battery and the unread dot, and no tap action.

The face owns no task, timer or service call. It reads a per-frame `face_ctx` that the home screen builds (`nos/apps/sys_home.c:465-490`) and keeps only animation state in RAM. Its only saved effect is the face index that the picker writes when GLYPH is chosen (`nos/apps/sys_faces.c:372-376`).

## Features

- **Analog time.** Hour and minute hands are tapered capsules with round caps and a hub (`nos/faces/face_glyph.c:148-154`, `:232-235`). Both angles come from `c->dt` (`:41-46`). The minute hand clicks forward in 4 frames when the minute changes by exactly +1 with no gap over 500 ms (`:79-83`, `:99-103`, `:208-212`).
- **Second hand.** One red dot on radius 202, positioned from `dt.second + dt.millis / 1000` (`:240-243`). It is drawn only when not in AOD (`:237`). Its colour is `fk_red(c, C_PRI)`, which is red normally and turns white while the REC chip is up (`nos/faces/face_kit.h:151-154`).
- **Tick ring.** 60 dots at radius 218 (`:25`, `:158-180`). Hour dots are 8 px, the 12 o'clock dot is white and 10 px, and the others are 4 px in `C_DIS`. Each dot's size is set by `full`, which then multiplies `f` (`:169-170`, `:178`).
- **Build-in.** The ring fills clockwise, 6 dots a frame, over 12 frames (`:202`, `nos/faces/face_kit.h:31`). The date and labels fade in over 6 perceptual steps, and the second dot snaps in at frames 9-11 (`:88-90`).
- **Date label.** Shown as "SUN 11" from `fk_date` (`nos/faces/faces.c:107-109`). The weekday is three letters in English and the day is the day of month. There is no month and no format setting.
- **Date window.** The window sits at 3 o'clock (right-aligned at x 388, baseline y 239) and moves to 9 o'clock (x 78, left, y 239) or 6 o'clock (centred, y 330) when a hand covers it (`:57-60`, `:65-75`). A 4-frame cross-fade plays on each move (`:84-87`, `:110-112`). The 12 o'clock window is declared but cannot be chosen (see Gaps).
- **Battery line.** Drawn under the date at y+24 (`:190`). It shows "NN%" with a bolt when charging, and shows only when charging or at 20% or below (`nos/faces/faces.c:582-599`). In AOD it shows only below 10% and not charging (`:582-585`).
- **Unread dot.** An 8 px white dot 14 px left of the date's left edge (`:184-194`, `nos/faces/faces.c:576-580`). It fades in 4 perceptual steps (`:552-566`). In AOD it becomes a 6 px `C_SEC` dot at the same place (`:565-569`).
- **AOD variant.** Only the 12 hour dots (5 px, `C_DIS`) are drawn (`:171`). The hands are hollow outlines (`:232-233`), the date is `C_DIS`, and the whole face drifts up to 4 px a minute (`nos/faces/faces.c:20-26`, `:57-64`).
- **Wake from AOD.** The outline hands fill in over 6 frames and the date and second dot fade in over the last frames (`face_glyph.c:14-17`, `nos/faces/faces.c:458-490`).

## Reads

Per-frame context, built by `build_ctx` (`nos/apps/sys_home.c:465-490`). Glyph reads only the fields listed here.

- `c->dt` (hour, minute, second, millis, weekday, day) comes from `hal_get_datetime()` (`nos/apps/sys_home.c:468`). On the board that is the ESP-IDF system clock, read with `gettimeofday` and `localtime_r` and latched once per frame by `hal_frame_begin` (`firmware/main/hal_esp32.c:117-146`, `:132`).
- `c->battery` comes from `svc_sys()->battery_pct`, falling back to `hal_battery_percent()` when that is 0 (`nos/apps/sys_home.c:474`; `nos/svc.c:986`, `:991`; `firmware/main/hal_esp32.c:392`).
- `c->charging` comes from `svc_sys()->charging`, which is `hal_power.external_power == 1` (`nos/apps/sys_home.c:475`; `nos/svc.c:992`).
- `c->unread` comes from `svc_notif_unread()`, the count of notifications with `read == 0` (`nos/apps/sys_home.c:476`; `nos/svc.c:238-242`).
- `c->aod` is `nos_aod && !aod_dimming_out()` (`nos/apps/sys_home.c:484`). `c->enter` is the build-in progress from `H.enter_t0` over `ENTER_S` 0.40 s (`:55`, `:280`, `:485`). `c->t` is `nos_t`. `c->rec` is `H.rec_raw` quantised to quarters (`:488`), fed by `sys_rec_state()` (`:150-165`).
- `hal_millis()` drives the 500 ms stale test and the wake-skip test (`nos/faces/faces.c:460`).
- `hal_big_alloc()` provides the shared PSRAM dot-list cache on first use (`nos/faces/faces.c:242`).
- Shared kit helpers call `ui_text_width`, `dot_icon` (the bolt) and `ui_label` (`nos/faces/faces.c:587-599`).

Populated for every face but not read by Glyph: steps and goal (`nos/apps/sys_home.c:471-472`), heart rate (`:473`), weather (`:477-480`), next alarm (`:482`), and `c->lite` (`nos/faces/face.h:21`). The picker's lite level is never applied here (`nos/faces/face_glyph.c` has no `fk_ghost` call).

`face_glyph.c` itself contains no `svc_*`, `prefs_*` or `hal_*` call. It reads no preference. `nos_prefs.hour24` (the Settings "24-hour clock" toggle, `nos/apps/set_settings.c:474`) is not used by GLYPH because it draws no digits. `nos_prefs.face_accent` (`nos/prefs.h:21`) has no reader anywhere under `nos/`.

## Writes

Glyph owns only module state in `S` (`nos/faces/face_glyph.c:32-39`): `last_hm`, `t0`, the wake tracker `fk_wake w`, `slot`, `slot_from`, `slot_t0` and the date cross-fade `fk_xfade date`. Everything else is framebuffer drawing (`:152-153`, `:234-235`, `:243`, `:204`).

It calls no service mutator, no prefs write and no HAL store, and it writes no SD or NVS data. It does not mark notifications as read. Those are cleared only by the Notifications screen (`nos/apps/sys_notifs.c:276`, `:292`) or by opening one on the phone (`nos/svc.c:269-276`, `nos/apps/sys_notifs.c:1039`).

Shared state it touches through the kit (`nos/faces/faces.c`), which is not Glyph's own:
- `s_dc_epoch` is incremented in `fk_aod_update` (`:57-59`).
- The seconds clock `s_sk` (`:40-50`) and the AOD drift `fk_aod_dx/dy` (`:34`, `:57-64`) are updated.
- `s_unread_k`, the unread fade level, is one value shared by every face (`:552-559`).
- The PSRAM dot cache `s_dc` is allocated on first use and never freed (`:242`).
- `face_segment_warm()` runs whenever GLYPH ticks with `enter >= 1` (`:61`). It warms SEGMENT's font and sprites a little per frame.

The home context `H.c` is rebuilt every frame by `build_ctx` (`nos/apps/sys_home.c:465-490`). The picker rebuilds it through `sys_home_ctx()` (`:493-497`).

## Background behaviour

- **No task of its own.** `tick` runs only from `home_tick` (`nos/apps/sys_home.c:625`). Home ticks when it is the top screen or sits under a transition (`nos/nos.c:2101-2110`). While another app covers home, Glyph's state is frozen. On return, a gap over 500 ms sets `stale`, so the morphs settle instead of replaying (`nos/faces/faces.c:458-460`, `nos/faces/face_glyph.c:97-105`).
- **Screen on (PWR_ON).** The UI frame loop runs at the motion clock, `NOS_FPS` 40 (`nos/render_config.h:8`). Glyph ticks and draws every frame. Services run each frame through `nos_bg_tick` (`nos/nos.c:2341`, `nos/apps/registry.c:62-66`).
- **AOD (PWR_AOD, PWR_SENSOR).** `nos_frame_run` runs once per second (`firmware/main/nos_main.c:2114-2121`), so Glyph ticks and draws its AOD variant at 1 Hz. The loop then sleeps 20 ms (`:2127`).
- **Screen off (PWR_OFF).** No UI frame runs and Glyph does not draw. The CPU stays on at 80 MHz (`firmware/main/nos_main.c:2091-2094`). Only `services_tick` runs, once a second (`:2094`), which calls `svc_tick` and its 1 s `sys_tick` (`nos/svc.c:987-996`, `:1303-1320`).
- **Battery sampling.** The `board-sense` task (priority 2, 4 KB stack, `firmware/main/board.c:1036`) calls `hal_esp32_sample_power()` once a second (`:1021`). It runs independently of the screen.
- **Clock.** The system clock runs continuously. The PCF85063 RTC is restored once at boot (`firmware/main/board.c:847-890`) and written back when the clock changes (`:747-751`, `rtc_maintain` at `:801`).

## Persistence

- Glyph persists nothing. Its state, the unread fade and the date windows are RAM only and reset at boot.
- The face choice is `nos_prefs.face` (`nos/prefs.h:20`). The picker writes it only when the choice changes (`nos/apps/sys_faces.c:372-376`). `prefs_save()` marks it dirty (`nos/prefs.c:260-265`), and the checkpoint goes through `hal_store_save_checked("nosprefs")` (`nos/prefs.c:349`).
- On the 1.43-B build that checkpoint is an alternating, checksummed SD bank at `/sdcard/watch/settings.{0,1}.bin` (`nos/prefs_disk.c:32`, `firmware/main/hal_esp32.c:236-256`). An NVS mirror is written only when the SD write is allowed (`firmware/main/hal_esp32.c:250`).
- The unread count is RAM only. `nos/svc.c:8` lists only `nosalarm` and `nosact` as persisted, and no notification store is written. The dot is gone after a reboot even if the phone still holds the messages.
- The time survives power loss through the PCF85063 (`firmware/main/pins.h`, B branch `BOARD_HAS_RTC 1`), restored at boot as above.

## Navigation

- **Shown on home page 0.** The face is `nos_faces[nos_prefs.face]` (`nos/apps/sys_home.c:271-275`). The pager moves from it to the widget and app pages.
- **Taps.** `face_glyph` has no tap handler (`nos/faces/face_glyph.c:253`, the last field is 0). Home calls `f->tap` only when it is set (`nos/apps/sys_home.c:1126-1127`), so a tap on the face does nothing. The only tap that does something on the face is on the REC chip while recording (`:1119-1124`, `:885`).
- **Long press.** Pushes the face picker in place (`nos/apps/sys_home.c:1139-1141`). See [[Face picker]].
- **Settings.** The "Watch face" row (`nos/apps/set_settings.c:461`) pushes the same picker (`:1852`). The row shows the current face name (`:576`).
- **Opens.** Nothing. The face has no `tap` target and opens no screen.
- **Opened from.** Home page 0 when `nos_prefs.face` is 2, and the picker (`nos/apps/sys_faces.c:372-376`) and Settings through the picker.

## Gaps

1. **[medium] The clock is not validated.** When no time source has set the clock, the board sets it to epoch 0 (`firmware/main/board.c:884-886`). `hal_datetime` has no valid flag, and nothing in `face_glyph.c` or `nos/faces/faces.c:107-109` checks one. The face therefore shows a 1970 date and an arbitrary time with no warning. The Settings "Set time" screen (`nos/apps/set_settings.c:476`, `:2272`) has the only "time not set" message (`:2370`). Impact: a new or power-cycled watch with no sync shows a confident wrong time and date.

2. **[medium] The 12 o'clock date window is unreachable, and the hysteresis comment is not what runs.** The window is declared at `nos/faces/face_glyph.c:59-60`, and `slot_pick` returns `3` only as a fallback (`:74`). Each hand is within 24 degrees of at most one of the three slots, which are 90 degrees apart, so two hands block at most two slots and one of the three is always free. The comment (`:62-64`) says the current window is kept until a hand comes within 18 degrees. The loop (`:68-70`) instead returns the first slot clear by 24 degrees in the order 3, 9, 6. A read-only model of `slot_pick` with the same constants was run over every hour and minute for each current window. Slot 3 was never returned. In 1,896 of the 4,320 (current window, time) cases where the current window was clear by 18 degrees, a different window was chosen, for example 9 o'clock to 3 o'clock at 00:00. Impact: the date cross-fades to the 3 o'clock window on a minute change even though nothing covers the current one. The shared-face docs, and the header comment's "then 12", describe a window that cannot appear.

3. **[medium] "Charging" on the B board is an inference.** `firmware/main/hal_esp32.c:323` sets `usb` from `usb_serial_jtag_is_connected()`, with the comment "SOF from a host, not charge current". Any enumerated USB host is therefore reported as external power (`:337`, `:350`). The other path is a rail reading of 4.4 V or more on three samples with ADC calibration (`:347-350`). The B pin map has no charge-status pin (`firmware/main/pins.h`, B branch: no `PIN_CHG_STAT`). `svc.c:992` maps external power to `charging`. Impact: the bolt and "NN%" can appear on a data-only PC connection. The line also disappears if the sample is stale, because `hal_power_get` clears `external_power` after 3 s (`firmware/main/hal_esp32.c:378-387`).

4. **[low] The battery figure is an estimate shown as fact.** On B the percentage is computed from the system rail divided by 3 on GPIO4 (`firmware/main/pins.h`, B branch `BAT_DIVIDER 3`). `build_ctx` ignores `battery_valid` (`nos/apps/sys_home.c:474`; `nos/svc.h:180`). On USB the percentage is only updated inside 3300-4300 mV and otherwise keeps the last value (`firmware/main/hal_esp32.c:354`). A zero reading hides the line silently (`nos/faces/faces.c:582-599`) rather than showing an unknown state.

5. **[medium, product] No complications.** The face shows no steps or goal, heart rate, weather or next alarm. The context carries all of them (`nos/apps/sys_home.c:471-482`; `nos/faces/face.h:14-20`), and Glyph reads none. A modern analog face normally shows at least one of these. The battery line appears only at 20% or below.

6. **[low] Dead tap.** `face_glyph` has no tap handler (`nos/faces/face_glyph.c:253`). Tapping the time, date or unread dot opens nothing. Alarms, timer, steps and notifications are not one tap away from the face. The shared-kit note records the same finding for five faces.

7. **[low] The unread dot carries no count, app or persistence.** It is a dot only. Notifications are RAM only (`nos/svc.c:8`), so it resets at reboot. It clears when the Notifications screen opens (`nos/apps/sys_notifs.c:276`, `:292`), not when a notification is read on the phone.

8. **[low] Dead preference paths.** The Settings "24-hour clock" toggle (`nos/apps/set_settings.c:474`) has no effect on GLYPH, which draws no digits. `nos_prefs.face_accent` (`nos/prefs.h:21`) is stored and has no reader anywhere under `nos/`, and no Settings row sets it. The second hand's red is hard-coded (`nos/faces/face_glyph.c:243`).

9. **[low] The picker's lite level is ignored.** `c->lite` is set for the dimmed or sliding picker (`nos/apps/sys_faces.c:140-151`) and is meant to fade unlit detail (`nos/faces/face.h:21`, `nos/faces/face_kit.h:138-146`). GLYPH never calls `fk_ghost`, so all 60 ticks stay at full strength while the picker dims or slides.

10. **[low] The date is minimal.** Day of month only, English weekday, no month, no locale and no date-format setting (`nos/faces/faces.c:107-109`).

11. **[low] Shared state couples faces.** Glyph's tick warms the SEGMENT face's caches (`nos/faces/faces.c:61`), and the unread fade (`:552-559`) and dot cache (`:242`) are shared with every other face. This is harmless for correctness but means GLYPH pays for another face's warm-up in its first frames.

12. **[info] Stale docs.** `docs/polish/faces_r1.md` items 2 and 3, and its line that no face shows the unread dot, describe an earlier build. The current code has the date windows (`nos/faces/face_glyph.c:57-60`), the hands at the true angle from frame 0 on wake (`:230-233`), and the unread dot (`:246-250`). The header's "glides at 0.7 px a frame" (`:11-20`) is not a constant in the code; the second hand is a continuous function of the millisecond clock (`:240-243`).

Related notes: [[Shell and faces]], [[Face picker]], [[Face registry and shared face kit]], [[Engine and scheduling]], [[Services hub]], [[Health and activity]], [[Sports and workouts]], [[Power and battery]], [[Storage and persistence]], [[Hardware and drivers]], [[Connectivity and voice]].
