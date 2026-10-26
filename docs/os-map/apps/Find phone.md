---
title: "Find phone"
id: app-find-phone
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Find phone

Source: `nos/apps/con_find.c` (493 lines). It holds two screens: `scr_find` (Find phone, `:364`) and `scr_find_watch` (the ring screen the phone triggers, `:492`). Read-only source review; nothing built, flashed or run on a board, and the serial port was not touched. Evidence is `repo-relative/path:line`. Where a comment and the code disagree, the code is cited.

## Summary

Find phone is a one-button app. The launcher lists it as "Find phone" (`nos/apps/registry.c:32`). A tap on the big round button sends `{"t":"findPhone","n":true}` to the phone over the Gadgetbridge NUS link (`nos/svc.c:965-970`), sets a RAM flag and shows RINGING with dot ripples. The next tap, a swipe back, or the side button stops it with `{"n":false}`.

The watch never learns what the phone did. It sends one command and sets its own flag. Nothing comes back from the phone, and nothing clears the flag except the screen or a BLE disconnect (`nos/svc.c:1224`).

Find watch is the reverse direction. The phone sends a `find` line with `n:true` (`nos/svc.c:1175-1179`). The service pushes the full-face ring screen (`nos/svc.c:649`, `:659-667`). A 2 s haptic and sound loop runs until a tap, a `find n:false`, or 120 s (`nos/svc.c:981`).

Neither screen owns persistent data. Their state is RAM (`nos/apps/con_find.c:192-208`, `:378-386`; `nos/svc.c:91-92`). This code writes no NVS, prefs or SD settings. The only persistent trace is the diagnostic action record for each phone-ring start and stop (`nos/svc.c:967`).

Status is **partial**, from source reading only. Sending, the ring display, tap stop, leave-stop and the find-watch loop are implemented. The watch has no delivery or stop acknowledgement from the phone, no watch-side time limit on the phone ring, and no offline path. The find-watch alert needs the external strap, which is not verified on B.

Hardware: the B panel and FT3168 touch (`CLAUDE.md` hardware table; B branch of `firmware/main/pins.h:35-67`), the ESP32-S3 BLE radio (`firmware/main/hal_esp32.c:1159-1162`), and PSRAM for the ripple lists (`firmware/main/hal_esp32.c:1595-1597`). The alarm output on B is strap-only. The haptic motor is on GPIO16 (`firmware/main/ext/ext_priv.h:33`). The speaker is on the strap I2S and amp pins (`firmware/main/ext/ext_priv.h:34-38`). Both are marked present without probing (`firmware/main/ext/ext_haptic.c:107-109`, `firmware/main/ext/ext_audio.c:343`). B has no built-in audio (`firmware/main/pins.h:64`, `BOARD_HAS_AUDIO 0`).

Doc drift:
- `docs/os-map/systems/Utility apps.md:94` says Find phone "calls `hal_find_phone()` when not connected". The screen never does. It refuses with FB_ERROR while disconnected (`nos/apps/con_find.c:347`), so only the service-level fallback can reach `hal_find_phone` (see Gaps 6).
- `nos/apps/con_find.c:252-253` says the phone "may stop ringing on its own (found, or its timeout)". Only a disconnect clears the flag (`nos/svc.c:1224`). The branch cannot fire while the link is up.
- `nos/apps/con_find.c:371` and `:450-451` describe a 36-frame "1.2 s" beat. At `NOS_FPS` 40 (`nos/render_config.h:7-8`, `nos/motion.h:25`, `nos/motion.c:31-35`) the beat is 0.9 s.
- `docs/polish/outconn_r1.md:372` says an offline tap gives no visual feedback. The button is not drawn offline (`nos/apps/con_find.c:293-296`). A tap that reaches it gives FB_ERROR (`:347`).

## Features

Find phone page (`scr_find`, title "FIND PHONE", `nos/apps/con_find.c:285`):
- **Round button.** 72 px radius at (UI_CX, 206) (`:175-176`, `:302-306`). Idle is a grey disc with the phone glyph. Ringing fills it white from the centre over 6 frames (`:190`, `:224`, `:242`, `:298-304`). The hit radius is 92 px (`:325-329`).
- **Start.** A tap on the button starts the phone ring and plays FB_TOGGLE_ON (`:346-350`). A disconnected tap plays FB_ERROR and does nothing (`:347`).
- **Stop.** A tap anywhere while ringing stops it and plays FB_TOGGLE_OFF (`:340-345`). The side button also stops it, by leaving the screen (see Navigation).
- **Ripples.** Three rings of 40 dots from radius 82 to 120 px, 1.8 s period, 7 px dots that shrink to about 3.9 px (`:179-181`, `:210`, `:110-143`). Colour steps down the grey ladder with age (`:65-72`, `:210`). Motion blur follows `ui_ripple_blur` (`:91-97`). Ripples run only while ringing or while the fill is still fading (`:262-263`).
- **Words.** "RINGING" sits at y 346 while ringing. The hint reads "TAP TO RING" or "TAP TO STOP" at y 382 (`:182-183`, `:308-322`). The old and new words change in a 7-frame sequence (`:184`, `:256-260`).
- **Offline page.** The BT glyph, "Phone not connected" and "Pair in Gadgetbridge as Bangle.js NOS" replace the button (`:293-296`). The page swaps with a 2-frame fade when the link goes or returns (`:28-63`, `:34`, `:223`, `:250`).
- **Link loss.** Ringing stops when the link drops (`:249`), with a feedback-free state change.

Find watch page (`scr_find_watch`, `nos/apps/con_find.c:492-493`):
- **Ring visuals.** Four white-to-grey rings of 56 dots from radius 98 to 224 px, 1.2 s period, 11 px dots (`:367-376`, `:399-403`). The ripples keep clear of the title and hint words (`:388-397`).
- **Disc and glyph.** A white disc at 84 px that pulses 8 px, with a bell glyph (`:383`, `:453`, `:468-469`).
- **Words.** "FIND WATCH" title and "TAP TO STOP" hint (`:471-472`).
- **Brightness.** Forced to 255 while the alert is active (`:420-421`). Restored on leave or stop (`:409`).
- **Stop.** A tap, the side button or a long touch stops it, plays FB_SUCCESS and pops the screen (`:475-490`, `:429-437`). A lost touch is ignored (`:477`). Swipe-back is blocked by SCR_NOBACK (`:492-493`).
- **Alert loop (service side).** HAPTIC_ALARM and SFX_ALARM every 2 s (`nos/svc.c:699-702`), and the display is kept awake (`:704`). Auto-stop after 120 s (`:981`).

## Reads

Service getters (`nos/svc.c`, declared `nos/svc.h`):
- `svc_sys()->phone_connected` (`nos/apps/con_find.c:222`, `:248`; `nos/svc.c:986`; field `nos/svc.h:181`). Written by `phone_tick` on a change (`nos/svc.c:1215-1225`) and set at init (`nos/svc.c:995`).
- `svc_find_phone_active()` (`con_find.c:220`, `:253`; `nos/svc.c:972`). Reads the RAM flag `s_find_phone` (`nos/svc.c:91`).
- `svc_find_watch_active()` (`con_find.c:416`, `:444`; `nos/svc.c:973`). Reads `s_find_watch` (`nos/svc.c:91`), which the GB `find` line sets (`nos/svc.c:1175-1179`).

HAL:
- `hal_phone_connected()` (`con_find.c:222`, `:248`; `firmware/main/hal_esp32.c:1159-1162`). True only when BLE is enabled, the clock is ready, NUS TX is subscribed and a central is connected.
- No other hal read. The screen does not read the clock, battery, RTC or IMU.

Prefs: none read directly. `con_find.c` includes `prefs.h` (`:22`) but uses no `nos_prefs` or `prefs_*` symbol. Indirectly, `nos_feedback` reads `nos_prefs.haptics` and `nos_prefs.sound` (`nos/nos.c:570-572`), and `nos_brightness_override(-1)` restores the user's brightness from `nos_prefs.brightness` (`nos/nos.c:580`).

Engine (timing and navigation only): `nos_t`, `nos_enter_t()`, `nos_top()`, `nos_transitioning()` (`con_find.c:214`, `:251`, `:262`, `:297`, `:446`), the motion helpers `mtw_*` and `m_frame_k` (`:224`, `:242`, `:452`), and `ui_label_width` (`:418-419`).

## Writes

Service and RAM:
- `svc_find_phone(on)` (`con_find.c:232`, `:249`, `:343`, `:348`). It sets `s_find_phone` (`nos/svc.c:968`). When `hal_phone_connected()` is true it sends `{"t":"findPhone","n":true|false}` through `txf` (`nos/svc.c:969`, `:150-156`). Otherwise, if `on`, it calls `hal_find_phone()` (`nos/svc.c:970`). The write itself is in `firmware/main/hal_esp32.c:1190-1222`, and it does not report success (see Gaps 1).
- `svc_find_watch_stop()` (`con_find.c:433`). Clears `s_find_watch` (`nos/svc.c:974`).
- Local statics: `F` (`con_find.c:192-208`, including the 32 KB ripple list `rip`), `W` (`:378-386`), `FIND_RIP` and `FW_RIP` specs (`:210`, `:403`).

HAL and output:
- `nos_feedback(FB_TOGGLE_ON/OFF/ERROR/SUCCESS)` (`con_find.c:345`, `:347`, `:350`, `:435`). This calls `hal_haptic` and `hal_sfx`, each gated by its pref (`nos/nos.c:557-573`).
- `hal_haptic_stop()` and `hal_sfx_stop()` in `fw_restore` (`con_find.c:407-408`). Called on stop, leave and enter.
- `nos_brightness_override(255)` on find-watch enter (`:421`), and `(-1)` on restore (`:409`). Both go to `hal_set_backlight` (`nos/nos.c:576-582`).
- `nos_keep_awake()` every frame while the phone is ringing (`:255`), and on every find-watch tick (`:441`). Each call stamps `s_last_input_ms` (`nos/nos.c:585`).
- `hal_big_alloc` for the two ripple lists, `2048 * 16 B` = 32 KB each (`:76`, `:102`; `docs/PSRAM_BUDGET.md:213`).

Navigation: `nos_pop()` on find-watch stop (`:436`) and on the timeout or phone-stop path (`:446`).

Not written: `nos_prefs`, NVS, the SD settings files and `hal_store`. A grep of `con_find.c` finds no `prefs_`, `nvs`, `fopen`, `hal_millis` or RSSI use.

## Background behaviour

- **Find phone has no task.** `find_tick` runs at `NOS_FPS` 40 Hz only while the screen is on top (`nos/nos.c:2101-2108`). The phone's own ring is the phone's to run. It stops when this screen leaves (`con_find.c:230-233`). It is also stopped by any `nos_push` over the screen, because `call_cover` calls `find_leave` (`nos/nos.c:316`, `:204-207`).
- **Screen off.** The PWR_OFF branch calls `nos_bg_tick(1.0f)` once a second and then `continue`s. It never calls `nos_frame_run`, so `find_tick` does not run (`firmware/main/nos_main.c:2091-2097`). A BLE drop is caught at the next 1 Hz service tick (`nos/svc.c:1215-1225`). The UI state follows on the next wake (`con_find.c:249`).
- **Keep-awake.** While ringing, the screen calls `nos_keep_awake()` each frame (`con_find.c:255`). The display does not auto-sleep until the user stops the ring or leaves.
- **Find watch is owned by the service.** `services_tick` runs `svc_tick` then `background_tick` (`nos/apps/registry.c:62-66`). That is `nos_bg_tick`, called once per UI frame (`nos/nos.c:2341`). The UI task is `app_main` on CPU 0 (`firmware/sdkconfig:1707`). The priority is the ESP-IDF default, since sdkconfig does not set it.
- **Find watch cadence.** The ringer and `call_find_tick` run in `svc_tick` (`nos/svc.c:975-983`, `:688-705`). That is 40 Hz with the screen on, 1 Hz with the panel off (`firmware/main/nos_main.c:2094`) and 1 Hz in AOD (`:2106-2116`).
- **Find watch alert (ringer_tick).** The kind is chosen in the order find, call, alarm, timer (`nos/svc.c:690-691`). For find it fires HAPTIC_ALARM and SFX_ALARM every 2000 ms (`:699-702`). The SFX plays regardless of Sound, because `kind != RING_CALL` short-circuits the prefs check (`:701`). Haptic has no prefs check (`:699`).
- **Find watch with the panel off.** `ring_screen` pushes `scr_find_watch` from the service (`nos/svc.c:659-667`, `:982`). `nos_push` calls `fw_enter` at once (`nos/nos.c:320`), which sets the 255 backlight. The PWR_OFF and AOD loops then switch to PWR_ON because the screen has SCR_KEEPAWAKE (`firmware/main/nos_main.c:2096`, `:2121`). Whether the panel lights before PWR_ON was not traced on hardware.
- **Find watch auto-stop.** 120 s from the first `find n:true` (`nos/svc.c:981`, t0 set at `:1177`). Re-sending `n:true` does not extend it.
- **Cover under the find-watch alert.** `fw_leave` stops the current buzz and tone (`con_find.c:424-427`). The ringer does not reset `s_ring_next`, so the next pattern fires at its next 2 s slot (`nos/svc.c:688-705`). The alert is not cancelled.
- **Radio and NUS.** The NimBLE host task fills the NUS ring. It is pinned to core 0 at priority `configMAX_PRIORITIES-4` with a 4 KB stack (`firmware/main/hal_esp32.c:1531-1533`; `firmware/sdkconfig:709-712`). The UI drains at most 8 lines per frame (`nos/svc.c:49`, `phone_tick` at `:1215`).

## Persistence

- Nothing from this app persists. `F` and `W` are RAM (`con_find.c:192-208`, `:378-386`), as are `s_find_phone`, `s_find_watch` and `s_find_watch_t0` (`nos/svc.c:91-92`). Reboot, restart and POWER OFF clear them. There is no save or load call in `con_find.c`.
- `s_find_phone` is cleared on BLE disconnect (`nos/svc.c:1224`).
- Diagnostics: every change of `s_find_phone` records `action_trace(DIAG_DOMAIN_FIND, START|STOP)` (`nos/svc.c:967`, helper at `:102-107`). The records go to `diag_log` (`nos/diag_log.c:229-243`). On the ESP build the diagnostics directory is `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`). The find-watch path records no action trace (`nos/svc.c:1175-1179`).
- `nos/diag_log.c:73-84` declares RTC_NOINIT retained storage. Whether these find action records reach it was not traced.
- Ripple lists are PSRAM buffers allocated once and kept for the boot (`con_find.c:102-104`; `docs/PSRAM_BUDGET.md:213`). If the allocation fails, `rip_build` returns with zero dots and the rings are silently absent (`con_find.c:104`).

See [[Storage and persistence]] for the diagnostics path and [[Power and battery]] for the keep-awake and full-brightness cost.

## Navigation

Find phone opens from:
- Launcher row "Find phone" (`nos/apps/registry.c:32`), opened with `nos_open` (`registry.c:45-50`). The iris transition plays FB_OPEN.
- Double press of the side button, which reopens the last launcher app (`nos/apps/sys_home.c:1157-1159`, `nos/apps/sys_launcher.c:139`, `nos/apps/registry.c:43`). This works only when Find phone was the last app opened from the launcher.
- Simulator: `find` (`sim/nos_sim.c:81`).
- Not reachable from the Control Centre, the Home pager or any face. No other `scr_find` or `DOT_ICON_FIND` use exists in `nos/` (`nos/apps/nos_apps.h:53`, `nos/dot.h:75`).

Find watch opens from:
- The service only. `call_find_tick` calls `ring_screen(&R_FIND, ...)` (`nos/svc.c:649`, `:975-983`), and `ring_screen` pushes the screen (`:659-667`). The trigger is the phone's `find` line (`nos/svc.c:1175-1179`).
- Simulator: `find_watch` (`sim/nos_sim.c:84`).

Find phone opens (pushes): none. The power sheet can cover it by IN_BTN_LONG (`nos/nos.c:953-956`). Any other `nos_push` also covers it (`nos/nos.c:316`).

Find phone exits:
- Tap while ringing (`con_find.c:340-345`).
- Swipe back. The screen has flags 0, so it is not SCR_NOBACK (`:364`; `nos/nos.c:852-861`). The leave stops the phone ring.
- Side button. `find_input` returns 0 for IN_BTN (`con_find.c:335-362`), so `dispatch_btn` calls `nos_home()` (`nos/nos.c:952`). The leave stops the ring.
- Link loss (`con_find.c:249`).

Find watch exits:
- Tap, IN_BTN or IN_LONG (`con_find.c:479-484`), which runs `fw_stop` (`:429-437`). The service flag clears and the screen pops.
- Phone sends `find n:false` (`nos/svc.c:1177-1178`). `fw_tick` then pops the screen (`con_find.c:444-447`).
- 120 s service timeout (`nos/svc.c:981`). `fw_tick` pops the screen (`con_find.c:446`).
- Swipe back is blocked (`con_find.c:492-493`). A touch cancel does nothing (`:477`).

## Gaps

1. **High. RINGING is optimistic. Nothing confirms delivery or the phone's state.** Evidence: `nos/svc.c:965-971` sets `s_find_phone` whether or not the send succeeds. `nos/svc.c:150-156` (`txf`) returns nothing. `firmware/main/hal_esp32.c:1190-1222` (`hal_phone_write_line`) returns void and drops the line after 20 ENOMEM retries, incrementing `s_nus_dropped`. `nos/svc.c:1175-1179` handles no inbound `findPhone` reply, and no inbound handler for it exists in `nos/`. User impact: the watch shows "RINGING / TAP TO STOP" while the phone may not have rung, and the user is not told.

2. **High. The find-watch alarm on a bare B may be a bright screen with no sound or buzz, and nothing says so.** Evidence: `firmware/main/ext/ext_priv.h:24-30` (EXT_BOARD_OK is 1 on B, so the strap drivers are compiled in), `:33-38` (strap pins). `firmware/main/ext/ext_haptic.c:107-109` ("A motor cannot be probed: it is 'present' when the driver came up"). `firmware/main/ext/ext_audio.c:343` ("cannot be probed; default on"). `firmware/main/pins.h:64` (`BOARD_HAS_AUDIO 0`). `CLAUDE.md` says the B strap "needs its own verified mapping". User impact: on a watch without the strap the only alert is the screen, and no indicator warns the wearer.

3. **Medium. The find-watch alarm ignores the Sound and Haptics settings.** Evidence: `nos/svc.c:699` calls `hal_haptic(HAPTIC_ALARM)` with no prefs check. `nos/svc.c:700-701` plays SFX_ALARM for RING_FIND, because `kind != RING_CALL` short-circuits before `nos_prefs.sound`. Other feedback is gated (`nos/nos.c:570-572`). User impact: a watch set to silent still plays the find alarm and buzzes.

4. **Medium. The watch never sees the phone stop, and the code that claims to handle it cannot run.** Evidence: `nos/apps/con_find.c:252-253` (the "found, or its timeout" branch). The only writers of `s_find_phone` are the screen and the disconnect at `nos/svc.c:1224`. User impact: after the phone user stops the ring on the phone, the watch still shows RINGING until the user taps.

5. **Medium. The phone ring has no time limit and keeps the display on.** Evidence: `nos/svc.c:965-971` (no timer). `nos/apps/con_find.c:255` calls `nos_keep_awake()` every frame while ringing. Compare the find-watch cap at `nos/svc.c:981` (120 s) and `RING_MAX_MS` at `nos/svc.c:47` (60 s). User impact: a forgotten ring holds the display on at the user's brightness until a tap or a leave.

6. **Medium. No way to ring the phone without a live NUS link.** Evidence: `nos/apps/con_find.c:222`, `:248` (connected gate). `:293-296` (offline page, no button). `:347` (FB_ERROR when disconnected). `nos/svc.c:969-970` (the companion path runs only when `hal_phone_connected()` is false). `firmware/main/hal_esp32.c:1239-1244` (`hal_find_phone` writes code 7 on MEDIA_CMD and needs `g_med_conn`). The companion path is reached only in the window between a link drop and the next `phone_tick`. User impact: with Gadgetbridge disconnected the screen cannot ring the phone at all.

7. **Low. Any overlay pushed over the screen silently cancels the phone ring.** Evidence: `nos/nos.c:316` (`nos_push` calls `call_cover(under)`), `nos/nos.c:204-207` (`call_leave`), `con_find.c:230-233` (`find_leave` sends `svc_find_phone(0)`). The pushers include the ring screens (`nos/svc.c:659-667`) and the power sheet (`nos/nos.c:953-956`). User impact: an alarm, a timer, a call or a find-watch request arriving while the user is on Find phone stops the phone's ring. On return the screen reads TAP TO RING with no notice.

8. **Low. A find-watch cover interrupts the buzz for up to 2 s.** Evidence: `con_find.c:405-410` (`fw_restore`) and `:424-427` (`fw_leave`) stop the pattern. `nos/svc.c:688-705` does not reset `s_ring_next` when the kind is unchanged. User impact: the alarm is briefly silent under an overlay and then resumes. It is not cancelled.

9. **Low. The find-watch alert forces full brightness and may light the panel early.** Evidence: `con_find.c:420-421` (255 override in `fw_enter`). `nos/nos.c:320` (`call_enter` runs inside `nos_push`, which `ring_screen` calls from the service). `firmware/main/nos_main.c:2091-2096` (the panel-off loop runs services). Whether the panel lights before PWR_ON was not traced on hardware. User impact: up to 120 s of full brightness regardless of the user's setting, a battery cost.

10. **Low. The disc pulse does not match its comment at 40 Hz.** Evidence: `con_find.c:371` and `:450-453` (36-frame beat, documented as 1.2 s). `nos/render_config.h:7-8` (NOS_FPS 40). `nos/motion.h:25` and `nos/motion.c:31-35` (`m_frame_k` uses M_FPS). At 40 Hz the beat is 0.9 s. The ring spacing is 0.3 s (`con_find.c:368`, `:370`). User impact: a cosmetic mismatch between the pulse and the ripples, from source only.

11. **Low (feature). No proximity, distance or last-seen indicator.** Evidence: `con_find.c` reads no RSSI and no phone location. No RSSI path exists in `nos/svc.c` for the phone link. User impact: the user cannot tell how close the phone is.

12. **Low (feature). Stopping the find-watch alarm tells the phone nothing.** Evidence: `con_find.c:429-437` (`fw_stop` calls `svc_find_watch_stop`, restore, feedback and pop, with no `txf`). `nos/svc.c:974`. User impact: the phone cannot show "found" or stop its own state.

13. **Low (feature). Find phone is not in the Control Centre or the Home pager.** Evidence: `scr_find` appears only in `nos/apps/registry.c:32`, `nos/apps/nos_apps.h:53` and `sim/nos_sim.c:81`. `DOT_ICON_FIND` appears only in `nos/apps/registry.c:32`, `nos/dot.h:75` and `nos/dot_glyphs.c:1954`. User impact: the feature needs the launcher or a double press.

14. **Low. Dead state and silent fallback.** `W.was_active` is written at `con_find.c:416` and `:444` and never read. The phone-stopped branch at `:252-253` is unreachable (Gap 4). If `hal_big_alloc` fails, `rip_build` returns with no dots and the rings vanish silently (`:102-104`). User impact: none on the happy path. These make review harder.

Related: [[Services hub]], [[Connectivity and voice]], [[Shell and faces]], [[Engine and scheduling]], [[Power and battery]], [[Storage and persistence]], [[Hardware and drivers]].
