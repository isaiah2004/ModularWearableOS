---
title: "Face picker"
id: sys-faces
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Face picker

Source: `nos/apps/sys_faces.c` (447 lines), screens `scr_faces` (`nos/apps/sys_faces.c:443`) and `scr_faces_home` (`nos/apps/sys_faces.c:447`). Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from source reading only, not from the device.

## Summary

The face picker is the full-screen page that swaps the watch face. It shows the nine live faces at full size, 466 x 466, with no scaling (`nos/apps/sys_faces.c:3`, `:276-286`). They page horizontally with home-style physics. A tap on the glass chooses the page the face is on. The hardware button cancels. The only persisted effect is `nos_prefs.face`, one byte in the preferences blob (`nos/apps/sys_faces.c:373-375`).

The picker owns no task, no timer and no service. It is a `nos_screen` with a tick and a draw, run by the UI loop only while it is the top screen (`nos/nos.c:2101-2110`). Its data comes from the same home face context that home builds, read through `sys_home_ctx()` (`nos/apps/sys_faces.c:136-137`, `nos/apps/sys_home.c:493-497`). It calls no `svc_*`, `hal_*` or `prefs_*` function directly, except `prefs_save()` (`nos/apps/sys_faces.c:375`).

Entry points: long press on the home face (`nos/apps/sys_home.c:1135-1141`), the ORRERY face's 3 s hold (`nos/faces/face_orrery.c:1253-1255`, `nos/apps/sys_home.c:1008-1013`), the Settings "Watch face" row (`nos/apps/set_settings.c:461`, `:1852`), and the serial bench command `open faces` (`firmware/main/nos_main.c:1557-1560`, `:924-931`).

The face list is a compile-time `const` table of nine faces (`nos/faces/faces.c:13-17`). DOT is index 0 and is the default (`nos/prefs.c:148`). There is no install, reorder, hide, or phone-side selection path.

Related: [[Shell and faces]] (home pager, Control Center, the face host), [[Engine and scheduling]] (screen stack, tick dispatch, sleep policy), [[Storage and persistence]] (prefs checkpoint, SD banks, NVS mirror, diagnostics), [[Hardware and drivers]] (optional haptic motor and audio, RTC restore), [[Services hub]] (the svc getters the faces read), [[Health and activity]] (steps and heart values on the faces), [[Power and battery]] (idle timeout, AOD and sensor-idle collapse, low battery), [[Connectivity and voice]] (REC chip sources: memo, IMU, track), [[Sports and workouts]] (REC chip shows a running workout).

## Features

- **Nine live faces, paged.** The list is DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE, ORRERY (`nos/faces/faces.c:15-16`). Each face's name comes from its definition: `nos/faces/face_dot.c:155`, `face_matrix.c:387`, `face_glyph.c:253`, `face_split.c:240`, `face_life.c:252`, `face_segment.c:623`, `face_inverted.c:176`, `face_tape.c:275`, `face_orrery.c:2555`.
- **1:1 finger tracking.** Drag position is the grab point minus the finger travel, clamped with a rubber band at both ends (`nos/apps/sys_faces.c:406-412`). Ends are `0` and `(nfaces-1) * 466` (`:63-64`).
- **Release picks the page.** A release faster than 350 px/s moves one page in that direction, otherwise the nearest page. The target is clamped to one page from where the grab started (`nos/apps/sys_faces.c:77-87`).
- **Settle.** A pixel spring with `SPRING_PAGE` (`nos/motion.h:44`) that ends with a 1 px snap and never overshoots (`nos/apps/sys_faces.c:66-75`).
- **Tick per page.** Each page change fires `FB_TICK` (`nos/apps/sys_faces.c:135`). That is a haptic tick with no sound (`nos/nos.c:557-574`).
- **Edit frame.** A rim on the glass edge at radius `UI_R - 3.5` (`nos/apps/sys_faces.c:311-313`), a name pill with the face's `name` (`:269-273`, `:316-318`), and page dots at y 440 (`:320-341`). The pill is 34 px tall at y 396 (`:37-38`).
- **Tap chooses.** Tap is accepted only when the pager is not moving or settling (`nos/apps/sys_faces.c:418-421`, `:356-364`). It starts a 6-frame fade (`CLOSE_S`, `:41`), then `finish()` saves and pops (`:370-384`).
- **Button cancels.** The button (`IN_BTN`) starts a mechanical cut back to the original face (`CANCEL_S` = `M_CUT_FRAMES` = 6 frames, `nos/apps/sys_faces.c:44`, `nos/motion.h:128`), with the frame fading out alongside. Nothing is saved (`nos/apps/sys_faces.c:426-437`, `:372`).
- **Opens in place from home.** It is pushed with `TX_NONE` (`nos/apps/sys_home.c:1140`). Its first frame equals home's. The frame builds in over `OPEN_S` = 8 frames, 0.2 s at 40 fps (`nos/apps/sys_faces.c:40`, `:237-256`).
- **Opens from Settings.** It is pushed with `TX_PUSH` (`nos/apps/set_settings.c:1852`). The face stays dimmed and the frame fades in over `SET_OPEN_S` = 9 frames (`nos/apps/sys_faces.c:42`). Going back is a slide.
- **REC chip under the picker.** On a home-opened picker the REC chip is drawn on the face home opened on, and gives way as the frame builds (`nos/apps/sys_faces.c:287-291`).
- **Touch is not routed to the face.** `f_input` never calls `nos_face_input`, so ORRERY's own touch handling is off inside the picker. This is what the code does, not an oversight the comments describe (`nos/apps/sys_faces.c:386-441`; no `nos_face_input` call in the file).

## Reads

Own state and registry (`nos/apps/sys_faces.c`):
- `nos_prefs.face`, at open (`:92`) and at finish (`:373`, `:379`).
- `nos_face_count` and `nos_faces[]` (`nos/faces/face.h:38-39`, `nos/faces/faces.c:15-17`). Per face: `name` (`:269`), `draw` (`:284-286`), `tick` (`:155`). The face's `enter` hook is never called by the picker (no `->enter` call in the file).
- `nos_face_style()` for `FACE_LIGHT` and `FACE_FULL_BLEED` (`nos/faces/faces.c:814-818`, used at `nos/apps/sys_faces.c:294`, `:299`, `:310`, `:337`).
- `nos_depth()` to decide `on_home` (`nos/nos.c:94`, `nos/apps/sys_faces.c:97`).
- `nos_enter_t()` and `nos_t` (`nos/nos.c:129`, `nos/apps/sys_faces.c:95`).
- Input types `IN_CANCEL`, `IN_DOWN`, `IN_DRAG_START`, `IN_DRAG`, `IN_DRAG_END`, `IN_TAP`, `IN_UP`, `IN_BTN` (`nos/apps/sys_faces.c:386-441`). `IN_BTN_LONG` and other types return 0 (`:438-439`) and reach the engine.
- Animation helpers only (`m_clamp`, `m_ink`, `m_cut`, `msp_*`, `mf_at`). No data.

Face context, rebuilt every picker frame by `sys_home_ctx()` (`nos/apps/sys_faces.c:136-137`; `nos/apps/sys_home.c:493-497`, `build_ctx` at `:465-490`):
- `hal_get_datetime()` (`firmware/main/hal_esp32.c:142-146`). It reads the ESP-IDF clock through `gettimeofday` and `localtime_r` (`:116-127`). It is not an RTC read per call.
- `svc_activity()`: `steps`, `goal` (`nos/svc.c:438`; `nos/svc.h:95-102`). Steps fall back to `svc_sys()->steps_today`.
- `svc_sys()`: `battery_pct`, `charging`, `steps_today` (`nos/svc.c:986`; `nos/svc.h:176-185`). Battery falls back to `hal_battery_percent()` (`nos/apps/sys_home.c:470-473`).
- `nos_prefs.step_goal_100`, the goal fallback of 8000 steps (`nos/prefs.h:26`, `nos/prefs.c:155`).
- `svc_heart()->bpm_last` (`nos/svc.c:581`; `nos/svc.h:104-113`).
- `svc_notif_unread()` (`nos/svc.c:238-242`).
- `svc_weather()`: `valid`, `temp_c`, `cond` (`nos/svc.c:337`; `nos/svc.h:80-88`).
- `svc_alarm_next()`: minutes to the next alarm (`nos/svc.c:768`; `nos/svc.h:129`).
- `nos_aod`, `H.enter_t0`, and `H.rec_raw` (`nos/apps/sys_home.c:465-490`).

REC chip clock, run from the picker's tick while on home (`nos/apps/sys_faces.c:123`; `nos/apps/sys_home.c:509`):
- `rec_step()` calls `sys_rec_state()` (`nos/apps/sys_home.c:499-507`, `:150-171`). Sources: `svc_memo_active()` and `svc_memo_elapsed_ms()`, `hal_imu_record_get()`, `workout_get()`, `svc_track_active()` and `svc_track_label()`.

The two visible faces are ticked and drawn with a copy of that context (`nos/apps/sys_faces.c:152-156`, `:278-286`). Each face's own reads are outside this card.

## Writes

- `nos_prefs.face = (uint8_t)p` only when it differs from the stored value (`nos/apps/sys_faces.c:373-374`). `p` is `F.page` on a choose and `F.orig` on a cancel (`:372`).
- `prefs_save()` only sets the dirty flag (`nos/prefs.c:260-265`). The write happens later in `prefs_tick` and `prefs_flush` (`nos/prefs.c:331-384`), through `hal_store_save_checked("nosprefs")` (`firmware/main/hal_esp32.c:228-256`).
- Diagnostics: `diag_action_request(DIAG_DOMAIN_FACE, DIAG_ACT_SET, page)` (`nos/apps/sys_faces.c:359`), `diag_action_context(..., diag_log_name_id("FACES"), F.orig)` (`:360`), `diag_action_result` as CANCELLED on leave (`:102`) and as CANCELLED, SUCCESS or FAILED in `finish()` (`:377-381`). Records are queued in RAM, then appended to SD by `diag_log_service` (`nos/diag_log.c:229-243`, `:460-500`).
- Feedback: `FB_TICK` on page change (`:135`), `FB_SUCCESS` on choose (`:363`), `FB_BACK` on cancel (`:429`). Mapped in `nos/nos.c:557-574`.
- Navigation: `nos_pop_with(TX_NONE)` when opened over home (`:382`), `nos_pop()` when opened from Settings (`:383`).
- RAM: the `F` struct, including a copy of the face context (`:46-61`). `build_ctx()` also writes home's `H.c` every picker frame (`nos/apps/sys_home.c:465-490`, `:493-497`). Home's `H.face_idx` is not written by the picker.
- No file, NVS key or SD path is written directly from this file.

## Background behaviour

- **The picker runs nothing when it is closed.** It has no task, timer, ISR or service registration. Its tick and draw run only when it is the top screen, from the UI frame (`nos/nos.c:2101-2110`, `:2347`; the UI loop is `app_main` on core 0 per `firmware/main/nos_main.c:1-3`).
- **While open, home is not ticked.** `tick_screens()` ticks only the top screen when idle (`nos/nos.c:2101-2110`). That is why the picker ticks the REC clock itself (`nos/apps/sys_faces.c:123`).
- **Cadence.** The screen presents at `NOS_PRESENT_FPS` 60 (`nos/render_config.h:13`), capped by the `fps_cap` preference (default 60, `nos/frame_cap.h:5`). Motion clocks run at `NOS_FPS` 40 (`nos/render_config.h:8`). Each frame ticks two faces, the one at rest and the next (`nos/apps/sys_faces.c:152-156`). It draws one full-size subview at rest and two during a slide (`:278-286`). During a slide the faces drop their ghost layers (`lite`, `:140-151`).
- **Services keep running regardless.** `nos_bg_tick` is `services_tick`, which calls `svc_tick()` and `background_tick()` (`nos/apps/registry.c:62-71`). It runs once per frame while the screen is on (`nos/nos.c:2341`). In the screen-off loop it runs at 1 Hz (`firmware/main/nos_main.c:2091-2094`).
- **Idle timeout.** The picker does not set `SCR_KEEPAWAKE` (`nos/apps/sys_faces.c:443`, `:447`; no other reference in `nos/apps/sys_faces.c`). The idle timer is `nos_prefs.sleep_s` (default 15 s, minimum 5 s: `nos/prefs.c:149`, `:168`). Touch and button presses reset it (`nos/nos.c:946`, `:896`, `:2339`). When it fires, `firmware/main/nos_main.c:2080-2085` applies `power_idle_target()`:
  - Default `sensor_idle` = 1 (`nos/prefs.c:159`), so the target is `PWR_SENSOR`. `go_state` sets `nos_aod = 1` and calls `nos_home_now()` (`firmware/main/nos_main.c:704-705`). That collapses the stack to home and runs the picker's `f_leave`. The CANCELLED record is written, nothing is saved, and home shows the AOD face (`nos/nos.c:429-439`, `nos/apps/sys_faces.c:100-104`).
  - With `sensor_idle` = 0 and `aod` = 0 the target is `PWR_OFF`. `go_state` does not collapse the stack in that case (`firmware/main/nos_main.c:691-707`). The picker stays on the stack, is not ticked or drawn (`:2091-2103`), and resumes on wake.
  - Low battery goes to `PWR_OFF` from ON or from AOD/sensor (`firmware/main/nos_main.c:2036`).
- **Haptic and sound are synchronous.** `hal_haptic` is an `esp_timer` one-shot chain with no task (`firmware/main/ext/ext_haptic.c:1-2`). `hal_sfx` queues to the audio task only when audio is up (`firmware/main/ext/ext_audio.c:358-361`).

## Persistence

- **Field.** `nos_prefs.face` is byte 12 of the 52-byte `nos_prefs_t` blob, by declaration order (`nos/prefs.h:20`; size asserted at `nos/prefs.c:14-15`). It is in the edit list (`nos/prefs.c:34`), so it takes part in the explicit-edit overlay.
- **Primary store.** SD, two alternating checksummed banks, `/sdcard/watch/settings.0.bin` and `settings.1.bin` (`nos/prefs_disk.c:32`, `:19-22`, `:44`, `:87`, `:99`; directory `"/sdcard/watch"` at `firmware/main/hal_esp32.c:243`). Writes are fsynced and read back (`nos/prefs_disk.c:11`, `:81`).
- **Mirror.** NVS namespace `wos`, key `nosprefs`, written when the SD write is allowed (`firmware/main/hal_esp32.c:177-181`, `:250`). This is the card-absent fallback.
- **Explicit-edit overlay.** NVS key `prefs_overlay`, verified after write (`firmware/main/hal_esp32.c:203-212`). It holds the full preference struct while edits are unflushed (`nos/prefs.c:118`, `overlay_checkpoint`).
- **Timing.** A save is debounced: 2 s after the last dirty mark, or 10 s after the first (`nos/prefs.c:381-383`). A failed save stays dirty and retries every 5 s (`nos/prefs.c:372-378`). In the ON state saves run only after touch and transition motion stop (`firmware/main/nos_main.c:2162-2164`). Persistence is blocked on entering ON (`firmware/main/nos_main.c:703`) and allowed in the OFF and AOD loops (`firmware/main/nos_main.c:2098`, `:2110`). Shutdown and restart call `prefs_flush()` (`firmware/main/nos_main.c:778`, `:796`, `:834`).
- **Diagnostics.** SD `/sdcard/watch/diagnostics` on device (`nos/diag_log.c:24`). Host builds use `build/nos_diagnostics` (`nos/diag_log.c:36`). Crash breadcrumbs sit in RTC retained memory (`nos/diag_log.c:74`).
- **Identity.** The stored value is an array index, not a face ID or name. Nothing in the record says which face it meant (`nos/apps/sys_faces.c:92`, `nos/apps/sys_home.c:271-276`).

## Navigation

- **Opened from.**
  - Home face page long press: `IN_LONG` with `fabsf(H.x) < 1` and no movement, pushes `scr_faces_home` with `TX_NONE` (`nos/apps/sys_home.c:1135-1141`). The rim charges from 0.15 s and reaches 3/8 at about 0.45 s (`nos/apps/sys_home.c:997-1000`, `:1054-1057`; `nos/nos.c:590`).
  - ORRERY face, 3 s hold: `home_open_faces()` (`nos/faces/face_orrery.c:1253-1255`, `nos/apps/sys_home.c:1008-1013`). ORRERY also takes touches first through `nos_face_input` (`nos/apps/sys_home.c:1026-1028`), so home's long press does not reach it there.
  - Settings, "Watch face" row (`nos/apps/set_settings.c:461`, `:1852`): pushes `scr_faces` with `TX_PUSH`.
  - Serial bench command `open faces`, through `screen_by_name` (`firmware/main/nos_main.c:924-931`, `:1557-1560`).
- **Opens.** Nothing. A long press on the side button opens the power sheet over the picker, because `f_input` returns 0 for `IN_BTN_LONG` (`nos/apps/sys_faces.c:438-439`; `nos/nos.c:944-957`).
- **Exits.**
  - Tap: `finish()` saves if the page changed, then `nos_pop_with(TX_NONE)` over home, or `nos_pop()` back to Settings (`nos/apps/sys_faces.c:370-384`).
  - Button: the same with the original face. Nothing is saved (`:426-437`).
  - Edge swipe: only on the Settings picker (`scr_faces` has no `SCR_NOBACK`, `:443`). The engine allows it from the left edge (`nos/nos.c:852-854`, `:861`). It leaves without saving, and `f_leave` records CANCELLED (`nos/apps/sys_faces.c:102`). From home the edge swipe is off (`scr_faces_home` has `SCR_NOBACK`, `:447`).
  - Idle timeout: see Background behaviour. Collapses to home without saving.
- **Returns to.** Home (`nos/apps/sys_home.c:552-553` picks up `nos_prefs.face` and invalidates the blur cache, with no rebuild) or Settings (the value row refreshes from `nos_prefs.face` in `update_values`, `nos/apps/set_settings.c:574-576`, `:687`).

## Gaps

1. **[medium] The picker closes itself while you browse.** It is not `SCR_KEEPAWAKE` (`nos/apps/sys_faces.c:443`, `:447`). With the default `sensor_idle` = 1 (`nos/prefs.c:159`), 15 s without a touch or button drops it to the AOD home (`firmware/main/nos_main.c:2080-2085`, `:704-705`; `nos/nos.c:429-439`). Impact: a pause to look at a face sends you back to home.
2. **[medium] No face library.** The registry is a `const` array of nine compile-time faces (`nos/faces/faces.c:13-17`). There is no install, download, remove, reorder, favourite or hide. A text search of `gb.c` and `host_request.c` finds no face command, so a phone cannot choose the face either. Impact: a modern-watch expectation is unmet.
3. **[medium] No per-face options.** No accent colour, style or complication choice. `face_accent` is persisted (`nos/prefs.h:21`, `nos/prefs.c:34`) and nothing reads or writes it outside the field list. The Settings and developer note already calls it dead (`docs/os-map/systems/Settings and developer.md:27`, `:200`), and the source agrees.
4. **[low] The saved choice is an array index.** Reordering or inserting a face in `nos/faces/faces.c:15-16` silently changes what every saved index shows. `normalize()` does not clamp `face` (`nos/prefs.c:165-191`). A stale index falls back to DOT on home (`nos/apps/sys_home.c:271-276`) and to 0 in the picker (`nos/apps/sys_faces.c:92`), without rewriting the stored value. The Settings row falls back the same way (`nos/apps/set_settings.c:575`).
5. **[low] The choice takes up to 2 s to reach storage.** The picker closes at once, but the save is debounced for 2 s and can wait up to 10 s (`nos/prefs.c:381-383`). In the ON state it waits for motion to stop (`firmware/main/nos_main.c:2162-2164`). A reset or battery loss inside that window loses the change. Clean shutdown flushes (`firmware/main/nos_main.c:778`, `:796`).
6. **[low] Feedback is silent on a bare 1.43-B.** The haptic needs the coin motor driver on IO16, which is only up when the driver initialises (`firmware/main/ext/ext_haptic.c:114-116`; `firmware/main/ext/ext_priv.h:33`). Sound needs the audio module (`firmware/main/ext/ext_audio.c:358-361`). `CLAUDE.md` records that B has no built-in audio. Tick, success and back are therefore silent unless the optional hardware is present.
7. **[low] Discoverability.** There is no on-screen label that the long press opens faces. The only cue is the rim charging on home (`nos/apps/sys_home.c:997-1000`).
8. **[low] Stale doc on the edge swipe.** `docs/polish/faces_r3.md:112` says the `sys_faces.c` header still says the left-edge swipe cancels. The current header (`nos/apps/sys_faces.c:12-16`) says the edge swipe applies only from Settings, and the code matches it (`:447`). From home there is no edge-swipe exit by design. The polish note is out of date, but the gap it describes is intentional.
9. **[low] Previews show the clock's state, not a real time.** The faces read the ESP-IDF clock (`firmware/main/hal_esp32.c:116-146`). If no RTC, USB, BLE or NTP source has set it, the clock is set to epoch 0 (`firmware/main/board.c:881-886`). The picker then previews faces at 1970 time until a time source arrives.
10. **[info, unverified on device] Stale face state after a long absence.** `docs/polish/faces_r2.md:153` reports a stale morph when paging to a face last seen minutes ago. The picker never calls a face's `enter()`. Most faces now guard with `fk_wake_stale()`, which marks a face stale after a 500 ms gap (`nos/faces/faces.c:457-466`; e.g. `nos/faces/face_inverted.c:72-73`, `face_split.c:64`, `face_life.c:84-91`). That looks addressed in source. ORRERY has no `fk_wake` hook (`nos/faces/face_orrery.c:1166` resets only its ephemeris frame). Its behaviour after a long gap is not traced here.
