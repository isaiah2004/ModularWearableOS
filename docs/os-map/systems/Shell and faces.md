---
title: "Shell and faces"
tags: [nos, os-map]
updated: 2026-10-23
---

# Shell and faces

## Summary

The shell is the set of `sys_*` screens in `nos/apps/` plus the face registry in `nos/faces/`. It is the part of NOS the wearer sees most. Everything here runs inside the single UI loop (the `app_main` task on core 0, see [[Engine and scheduling]]). None of it owns a task or timer. Data arrives by synchronous getter calls into the services hub ([[Services hub]]).

- `sys_boot` plays a ~1.4 s dot-matrix wordmark and hands over to `sys_home`.
- `sys_home` is the root screen. It is a 7-page horizontal pager: Activity, Heart, Weather, Music and Timer widgets, then the watch Face, then the Apps launcher page. A vertical drag from the face pulls Control Center (down) or Notifications (up). A long press on the face opens the face picker (`sys_faces`). The button long press opens `sys_power`.
- 9 faces are built (DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE, ORRERY). The simple faces are mostly clock-only. SPLIT, SEGMENT and INVERTED carry real data. ORRERY computes its own ephemeris.
- Face and widget data on device is real. The simulator uses a fake `SIM_DAY` table (`#ifndef ESP_PLATFORM`). Real-but-estimated values are called out in Gaps (kcal and distance are step multiples).
- Preferences (face, brightness, toggles) live in RAM `nos_prefs` and are checkpointed to SD with an NVS fallback. Notifications are RAM-only and are lost on reboot.

## Components

### sys-boot (`scr_boot`)
- Purpose: cosmetic boot animation, 5x7 dot-matrix "NOS" particles (MAXDOT 64, F_POP=43 frames at 30 fps) with `hal_sfx(SFX_BOOT)` and a tick haptic. Particles use `hal_big_alloc` (PSRAM).
- Where it runs: pushed by `nos_apps_boot` right after `nos_set_root(&scr_home)`; UI task, 30 fps.
- Cadence: one-shot, ~1.4 s. Any touch or button skips it. `finish()` calls `sys_home_build_in()` then `nos_pop_with(TX_NONE)`.
- Reads: nothing from services. Writes: nothing persistent. Not gated on readiness (prefs, services).
- Files: nos/apps/sys_boot.c:101 (enter, SFX), nos/apps/sys_boot.c:104 (finish), nos/apps/registry.c (nos_apps_boot).

### sys-home (`scr_home`)
- Purpose: root pager, face host, widget host, REC chip, prewarm scheduler, wake morph.
- Where it runs: UI task. `home_tick` every frame at 30 fps with the screen on, 1 Hz in AOD (`nos_aod=1`), never while the screen is off (PWR_OFF).
- Cadence details: face and all 5 widgets are ticked every frame even when hidden (`shown()` fires on the rising edge). The face is not ticked while the Apps page covers the glass. Even frames run `home_blur_tick`. Odd frames, when no interactive face is active, rotate `sys_launcher_prewarm` / `sys_notifs_prewarm` / `set_settings_prewarm` by `nos_frame % 3`. A gap over 1500 ms, or the `nos_prefs.sleep_s` timeout, is treated as a wake and calls `sys_home_build_in()`.
- Reads (in `build_ctx`, nos/apps/sys_home.c ~400-482): `hal_get_datetime`, `svc_activity()`, `svc_heart()->bpm_last`, `svc_sys()` (battery, charging; falls back to `hal_battery_percent()`), `svc_notif_unread()`, `svc_weather()`, `svc_alarm_next()`. REC chip (`sys_rec_state`, ~100-125): `svc_memo_active`, `hal_imu_record_get`, `workout_get`, `svc_track_active`.
- Writes: pager state and navigation (`nos_pull`/`nos_push`); it does not write `nos_prefs` directly.
- Input: face tap goes to `nos_face.tap`; IN_LONG opens `scr_faces_home`; IN_BTN toggles Apps page and face; IN_BTN_DOUBLE reopens the last app (`sys_launcher_last`); IN_AIR left/right gestures change page; vertical drag from the face with |x|<40 pulls Control or Notifications; REC chip tap opens the recorder app after 3 lit frames.
- Files: nos/apps/sys_home.c (1184 lines; `home_input` ~905-1160), nos/apps/sys_launcher_page.h.

### sys-widgets (home pages 2..6, owned by sys-home)
Five widget objects `k_wg[5]` in sys_home.c, implemented in app files. Per-complication data is in "Data in / data out".
- wg_activity (nos/apps/fit_activity.c:583), wg_heart (fit_heart.c:1025), wg_weather (out_weather.c:945), wg_music (con_music.c:969), wg_timer (time_timer.c:1212).
- Where: UI task, ticked every frame by home.

### sys-launcher (`scr_launcher` and Apps page)
- Purpose: rotary-dial list of `nos_app_list` (28 apps; MAXAPP 32; ROW_P 92). Icons and names are baked into 8-bit PSRAM sprites in resumable slices (`sys_launcher_prewarm`, budget 750 us / 16 steps). Fling blur has 8 variants (NBLUR). LAUNCHER_RESERVE 512 KiB. ROM-asset path via `native_menu_assets`.
- Where: UI task. Home owns the page lifecycle (`sys_launcher_page_enter/leave/tick/draw/input`); the standalone `scr_launcher` shares the code.
- Writes: `nos_open_icon(app.scr, ...)`, which sets `s_last_app` and uses the iris transition. Double button press retargets to the last app.
- Files: nos/apps/sys_launcher.c (1022 lines), nos/apps/sys_launcher_page.h, nos/apps/registry.c.

### sys-control-center (`scr_control`)
- Purpose: quick toggles, brightness, tools. Pulled down from the face (TX_DROP, SCR_HDRAG).
- Page 0: 11 tiles (WIFI, BT, DND, TORCH, AOD, SOUND, HAPTIC, LTE, GPS, RAISE, SETTINGS), header "HH:MM . bat%" or the REC chip, 10-segment brightness pill (min 5 %). Page 1 "QUICK TOOLS": FILE QUICK REPORT, ENERGY, BATTERY HISTORY, "LOGS / WRIST TUNING" (opens wrist tuning only).
- Reads: `nos_prefs`, `hal_ext_present()` (gates SOUND/HAPTIC/LTE/GPS), `svc_sys()`, `hal_get_datetime`.
- Writes: `nos_prefs.wifi/ble/dnd/aod/sound/haptics/lte/gps_auto/raise_wake/brightness` then `prefs_save()` (2 s debounce). LTE and GPS also call `hal_lte_enable` / `hal_gps_enable` directly. Brightness calls `hal_set_backlight` immediately.
- Consumers of the toggles: `radio_preferences` in firmware/main/nos_main.c (~953-962, applies wifi/ble); wake_service for raise; svc.c:230 and nos.c:519 for DND.
- Files: nos/apps/sys_control.c (752 lines; tile table :85; tool names :494).

### sys-notifications (`scr_notifs`, `scr_notif_detail`)
- Purpose: list of up to 20 phone notifications, swipe-left dismiss, CLEAR ALL, detail with OPEN ON PHONE. Pulled up from the face (TX_RISE).
- Reads: `svc_notif_count/get/revision`, `svc_sys()->phone_connected`, `hal_phone_connected`, `hal_epoch_seconds`. Marks all read on enter (`svc_notif_mark_all_read`).
- Writes: `svc_notif_dismiss` (DISMISS to phone), `svc_notif_clear`, `svc_notif_open_on_phone` (toast "No phone" when disconnected).
- Per-card fling-blur bake via `sys_notifs_prewarm`.
- Files: nos/apps/sys_notifs.c (1063 lines), store in nos/svc.c:60-270.

### sys-faces (`scr_faces`, `scr_faces_home`)
- Purpose: live full-size face picker. Ticks the faces in the two visible slots with a copy of the home context (`enter=1`, `lite` for ghost layers).
- Writes: `nos_prefs.face` and `prefs_save()` only if changed. Button cancels. Keeps the REC chip clock running via `sys_home_rec_tick`.
- Files: nos/apps/sys_faces.c (447 lines).

### sys-power (`scr_power`)
- Purpose: SLEEP / RESTART / POWER OFF sheet opened by button long press (`nos_power_screen`, nos.c:953). Battery hero from `svc_sys` / `hal_battery_percent`.
- SLEEP: `nos_home()` then `nos_host_power(0)` then `go_state(PWR_OFF)` (panel off, 80 MHz, 1 Hz `nos_bg_tick`). RESTART and POWER OFF are arm-then-tap (3 s ARM_S), then `nos_host_power(1|2)` after 0.8 s.
- Host side (firmware/main/nos_main.c:751-810): `shutdown_guard` (OTA, voice chat, IMU/audio recording), `prefs_flush`, `hal_ext_shutdown`, `board_prepare_sleep`, then `esp_restart()` or `esp_deep_sleep_start()` with ext0 wake on GPIO0.
- Files: nos/apps/sys_power.c (371 lines).

### eng-settings-shutter (helper, not a screen)
- nos/settings_shutter.c (194 lines): tent-kernel motion-blur "shutter" builder (kernel make, prefix rows, blit). Used only by nos/apps/set_settings.c for the Settings list blur. Contains the comment "Placeholder mask-class accounting, NOT a measured shutter target cost" (GFX_COST_MASK).

### eng-face-kit (face contract and helpers)
- nos/faces/face.h: `nos_face{name, enter, tick(face_ctx, dt), draw, tap}` and `face_ctx` (dt, steps, step_goal, bpm, battery, charging, unread, rec, lite, weather_valid, temp_c, wx_cond, next_alarm_min, t, aod, enter).
- nos/faces/face_kit.h: `fk_*` helpers (wake morph `fk_wake`, `fk_status`, `fk_unread`, `fk_date`, `fk_hour_disp`, `fk_aod_update`). nos/faces/face_ext.h: `nos_face_style`, `nos_face_input`, `nos_face_interactive`, `nos_face_press_rect`, `home_open_faces`, orrery tunables.
- nos/faces/faces.c (840 lines): registry order DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE, ORRERY. Default index 0 (DOT). Styles: INVERTED = LIGHT|FULL_BLEED; MATRIX/SEGMENT/GLYPH/LIFE/ORRERY = FULL_BLEED. `nos_face_input` and `nos_face_interactive` are ORRERY-only.
- AOD: `fk_aod_update` runs `sec_tick`, `face_segment_warm()` and `face_aod_shift` (Lissajous within +/-4 px, one step per minute).
- fk_status shows battery only when charging or <= 20 % (AOD: < 10 % and not charging). fk_unread is an 8 px red dot at (233,24).

### Faces
| id | file | data used | tap |
|---|---|---|---|
| face-dot | nos/faces/face_dot.c (155) | clock, date, battery status, unread dot | none |
| face-matrix | face_matrix.c (387) | clock, battery bar (`c->battery`, `charging`) | returns 0 always (:385) |
| face-glyph | face_glyph.c (253) | analog clock, date | none |
| face-split | face_split.c (240) | HR, steps, temperature, battery | HR to Heart, STP to Activity, TMP to Weather |
| face-life | face_life.c (252) | clock, date, 24 h ring | none |
| face-segment | face_segment.c (623) | steps, kcal, active minutes rings | rings and values to Activity |
| face-inverted | face_inverted.c (176) | steps line, battery | steps line to Activity |
| face-tape | face_tape.c (275) | clock only | none |
| face-orrery | face_orrery.c (2661) | ephemeris, orrery sub-modules | hold gestures, unlocked touch |

### face-orrery and sub-modules
- face_orrery.c: planets at real heliocentric longitudes from core/ephem.c using `unix_now() = hal_epoch_seconds() + millis`; if the epoch is 0 it treats local datetime as UTC (:1143-1148). Hold gestures: 0.75 s tilt, 1.5 s lock/unlock, 3 s face picker. Locked taps toast "HOLD 1.5 S TO UNLOCK". Unlocked: PAN / LOOK / TIME modes, zoom 0.5-4x, menu. AOD forces locked, home, now. Uses `hal_big_alloc` PSRAM heavily.
- orrery_card.c (2298): planet card. Textures loaded from SD by the "asset-read" task: `"%s/sky/%s.%s"` with `hal_sd_root()`, `.565` (1 MiB) or `.rgba`, plus earth_night and saturn_ring (:738-790). Falls back to flat tint when absent. `orrery_card_asset_poll` is called from the main loop even with the screen off.
- orrery_events.c (147): event list from the alignment scan plus flybys, comets and a hard-coded "BepiColombo reaches Mercury" (JD 2461365.5). Row tap calls `orrery_jump` and pops. Slice of at most 4 scan days per UI frame.
- orrery_data.c (4580): generated static flash sky pack, `od_pack_jd = 2461327.5`, built 2026-10-14. Belt 3999, small bodies 23, comets 21, crafts 16. Not updatable at runtime.

## How it runs

- Boot: `nos_apps_boot` runs `svc_init()`, `background_init()`, sets `nos_bg_tick = services_tick` (`svc_tick` + `background_tick`), sets `nos_power_screen`, sets root to home and pushes boot.
- Screen on (PWR_ON): the pacer drives frames at NOS_FPS 30. Each frame the engine calls `nos_bg_tick` first, so the service getters are fresh, then the active screen. Home ticks face and widgets. Draw is damage-tracked (stripes, `nos_subview`, page-blur bake cache).
- AOD / SENSOR: `nos_frame_run` once per second. Home ticks at 1 Hz. Screens flagged SCR_KEEPAWAKE force PWR_ON. 30 fps frames run only during the AOD entry ramp.
- Screen off (PWR_OFF): `nos_bg_tick(1.0f)` once per second, no screen ticks, no home ticks. Face data is not refreshed, but services (steps, HR, phone, alarms) keep running. Asset poll and prefs tick also run.
- Transitions: TX_DROP, TX_RISE, TX_IRIS, TX_PUSH, TX_NONE with spring `SPRING_PAGE` and constants FLICK_V 350, ENTER_S 0.40, PAGE_W 466.
- Related tasks (not owned here): "flush" core 1 prio 4, "touch" core 1 prio 5, "asset-read" prio 1 (ORRERY textures).

## Data in / data out

### Data in: complications and widgets

| Where | Field | Service getter | Refresh cadence | Real or placeholder |
|---|---|---|---|---|
| All faces, REC chip | time and date | `hal_get_datetime` | every frame (ms clock) | Real (RTC) |
| SEGMENT / INVERTED / wg_activity | steps, goal | `svc_activity()->steps`, goal from `nos_prefs.step_goal_100*100`; fallback `svc_sys()->steps_today` | `act_tick` 1 s; widget copies every frame | Real (`hal_activity_get`, persisted `nosact` every 10 min if dirty) |
| SEGMENT kcal ring | kcal | `svc_activity()->kcal` (fallback steps*0.04) | 1 s | Estimate: steps*0.04 (svc.c:445); goal hard-coded 400 (face_segment.c:266) |
| SEGMENT active-min ring | active_min | `svc_activity()->active_min` | 1 s | Derived from cadence > 60 steps/min; goal hard-coded 30 |
| wg_activity | km | `svc_activity()->km` | 1 s | Estimate: steps*0.75/1000 (svc.c:444) |
| SPLIT HR, wg_heart | bpm | `svc_heart()->bpm_last` | `heart_tick` every 60 s; widget refreshes when history age > 60 s | Real if a sensor sample exists; no age limit on bpm_last (svc.c:602) |
| wg_heart | age label | `svc_heart()->bpm_last_epoch` | on refresh | Real; shows "N MIN AGO" / "JUST NOW"; "NO HR SENSOR" without EXT_HR |
| SPLIT TMP, wg_weather | temp, cond | `svc_weather()` | `wx_tick` checked every 10 s; phone weather if < 3 h old else Wi-Fi Open-Meteo | Real; `valid` stays set when stale (> 1 h); widget shows STALE, face does not |
| faces (ctx only) | wx_cond, next_alarm_min | `svc_weather()->cond`, `svc_alarm_next()` | every home tick | Computed (sys_home.c:480-482) but consumed by none of the 8 simple faces |
| All faces, REC chip, Control, Power | battery %, charging | `svc_sys()` (fallback `hal_battery_percent`) | 1 s (`sys_tick`) | Real |
| All faces | unread dot | `svc_notif_unread()` | every home tick | Real, RAM store |
| wg_music | track, play state | `svc_music()`, `svc_music_cmd` | `music_tick` every frame; phone push | Real (Gadgetbridge); empty if no phone |
| wg_timer | timer / stopwatch / next alarm | `svc_timer()`, `svc_stopwatch()`, `svc_alarm_next()` | every frame | Real; falls back to quick-start circles |
| REC chip | memo / IMU / sport / nav | `svc_memo_active`, `hal_imu_record_get`, `workout_get`, `svc_track_active` | every home tick | Real |
| ORRERY | planet positions | core/ephem.c, `hal_epoch_seconds` | per frame | Real computation; local-time-as-UTC fallback if epoch is 0 |
| ORRERY | events, flybys, craft | static `od_*` pack (built 2026-10-14) | never | Static, ages (see Gaps) |
| ORRERY card | textures | SD `/sky/*.565`, `.rgba` | on open (asset-read task) | Real files if provisioned, else flat colour |
| sys-notifications | list | `svc_notif_*` | revision change | Real (phone), RAM only |
| sys-control-center | tile state | `nos_prefs`, `hal_ext_present()` | on open | Real; four tiles disabled on a bare B |

### Data out

| What | Target | Mechanism |
|---|---|---|
| face index | `nos_prefs.face` then `prefs_save()` | direct write (sys_faces `finish`) |
| CC toggles and brightness | `nos_prefs.*` then `prefs_save()`; `hal_set_backlight`, `hal_lte_enable`, `hal_gps_enable` | direct calls |
| notification dismiss / open | phone via `hal_phone_write_line` | `svc_notif_dismiss`, `svc_notif_open_on_phone` |
| music commands | phone | `svc_music_cmd` |
| timer start | `svc_timer_start` | wg_timer quick-start |
| power actions | `nos_host_power` in nos_main.c | callback (sleep, restart, deep sleep) |
| navigation | screen stack | `nos_push/pop/pull/open`, `nos_set_root` |
| SFX and haptics | HAL | `hal_sfx`, `nos_feedback` |

## Persistence

- `nos_prefs` (face, face_accent, brightness, toggles, sleep_s, units, goals): RAM, checkpointed by `prefs_save()` (2 s debounce, 10 s max dirty age) to SD `/watch/settings.0.bin` / `.1.bin` via `hal_store("nosprefs")`, NVS fallback when the card is absent. Failed saves stay dirty and retry every 5 s. `prefs_flush()` runs at shutdown. See [[Storage and persistence]]. nos/prefs.c:34 (field list), 260, 331-382; nos/prefs.h.
- Activity: `nosact` store written by svc.c every 10 min when dirty (5 s retry). Backs the Activity widget and SEGMENT.
- Heart history: 144-bin history from the HAL; svc copies it. See [[Health and activity]].
- Notifications: RAM only (`svc_big *B`, 20 entries). Lost on reboot or power off.
- Face internal state (ORRERY mode, zoom, lock, time offset) is RAM only; only the selected face index persists.
- Flash-resident: ORRERY sky pack, fonts, launcher ROM assets. SD read-only: `/sky/*` textures.

## Interactions

- [[Engine and scheduling]]: the shell is driven by the UI loop, the pacer and the power states (PWR_ON / AOD / SENSOR / OFF). Screen stack and transitions are engine facilities.
- [[Rendering pipeline]]: damage tracking, `nos_subview`, page-blur bake caches, SEGMENT/ORRERY sprites in PSRAM, settings shutter blur.
- [[Services hub]]: every data complication reads `svc_*` getters. Notifications, music, weather, timers live there.
- [[Health and activity]]: steps, kcal, active minutes, HR for SEGMENT, SPLIT, INVERTED and the Activity/Heart widgets.
- [[Sports and workouts]]: REC chip "SPORT / PAUSE" from `workout_get`.
- [[Power and battery]]: battery %, charging, power sheet, sleep/off flow, raise-to-wake toggle.
- [[Storage and persistence]]: prefs checkpoint, SD textures, notification store absence.
- [[Hardware and drivers]]: `hal_ext_present()` gating, backlight, SD, RTC epoch.
- [[Connectivity and voice]]: Wi-Fi/BT toggles, phone link for notifications, music, weather; shutdown guard for voice chat.
- [[Utility apps]]: widgets open Activity, Heart, Weather, Music, Timer; Control opens Torch, Battery; launcher lists the utilities.
- [[Settings and developer]]: Control "SETTINGS" tile, `set_settings` prewarm and shutter, Developer tools in the launcher.
- [[Calorie trace]]: face kcal is a plain steps*0.04 estimate, independent of the trace.
- [[Gaps]], [[OS overview]], [[Shell and faces]] (this note).

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| Medium | Notifications are RAM only | nos/svc.c:60-270 (`svc_big *B`, `SVC_NOTIF_MAX 20`) | Reboot, crash or power off loses all notifications and unread state |
| Medium | SPLIT HR row shows `bpm_last` with no age | nos/faces/face_split.c:76; nos/svc.c:602 | A stale reading displays as current and can turn red at >= 120 or < 40 |
| Medium | kcal and distance are step multiples; SEGMENT goals fixed | nos/svc.c:444-446; nos/faces/face_segment.c:266 | Kcal ring is not calorie burn; 400 kcal and 30 min goals cannot be changed; unrelated to workout and [[Calorie trace]] |
| Medium | ORRERY data is a frozen pack | nos/faces/orrery_data.c (`od_pack_jd 2461327.5`); orrery_events.c (BepiColombo) | Spacecraft and small-body data age; events list drifts out of date |
| Low | SPLIT TMP shows stale temperature without a cue | nos/faces/face_split.c:80-81; sys_home.c:480 | Up to 3 h old phone weather, or stale Wi-Fi data, looks current (widget shows STALE) |
| Low | Control tiles dead on bare B | nos/apps/sys_control.c:85 (`need` bits) | SOUND, HAPTIC, LTE, GPS are greyed and say "NO module" |
| Low | DND only silences banners and notify feedback | nos/svc.c:230; nos/nos.c:519 | Alarms, timers and calls still ring when DND is on |
| Low | "LOGS / WRIST TUNING" row opens only Wrist tuning | nos/apps/sys_control.c:494 | Sensor logs not reachable from that row |
| Low | Hidden widgets ticked every frame | nos/apps/sys_home.c (`home_tick`, k_wg loop) | UI-task time and power spent on invisible pages (also at AOD 1 Hz) |
| Low | SEGMENT warm-up runs from every face | nos/faces/faces.c (`fk_aod_update` to `face_segment_warm`); face_segment.c:323 | PSRAM and ~1 ms/frame used even if SEGMENT is never selected |
| Low | ORRERY depends on SD textures and RTC epoch | nos/faces/orrery_card.c:738-790; face_orrery.c:1143 | Missing `/sky` files give flat planets; epoch 0 shifts positions by the timezone offset |
| Low | `wx_cond`, `next_alarm_min` unused by the simple faces | sys_home.c:480-482 | Dead per-frame work; those faces show no alarm or condition |
| Low | Five faces have no tap target | DOT, GLYPH, LIFE, TAPE, MATRIX (face_matrix.c:385) | Date and clock taps do nothing |
| Low | Boot animation not tied to readiness | nos/apps/sys_boot.c | Home may appear before services have data |
| Low | Notification OPEN needs a phone | nos/apps/sys_notifs.c | Shows "No phone" toast otherwise |
| Info | SLEEP is panel off; POWER OFF is deep sleep on GPIO0 | nos/apps/sys_power.c; firmware/main/nos_main.c:751-810 | Not a true power cut; battery still drains slowly |
| Info | settings_shutter placeholder cost accounting | nos/settings_shutter.c (GFX_COST_MASK comment) | Sim cost model not measured; no runtime effect |
| Info | Legacy, not built | firmware/main/main.c, core/wos.c, core/apps/*, core/pip*.c | Dead code; ignore |
