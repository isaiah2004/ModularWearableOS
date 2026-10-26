---
title: "Navigate"
id: app-navigate
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Navigate

Source: `nos/apps/out_nav.c` (1118 lines). Read-only source review; nothing built, flashed or run on a board, and no serial access. Evidence is `repo-relative/path:line`. Track internals come from `nos/svc_track.c`, GPS from `firmware/main/ext/ext_gps.c`, compass from `firmware/main/ext/ext_mag.c`.

## Summary

Navigate is the outdoor GPS screen (`scr_nav`, `nos/apps/out_nav.c:1111-1112`; launcher tile "Navigate", `nos/apps/registry.c:29`). It does two things:

1. Shows live GPS. Before a fix it draws a sky plot. After a fix it shows a three-page pager: NOW, TRACK, BACK TO START.
2. Starts and stops a GPS track. The recording is not in this app. It lives in the track service `nos/svc_track.c`, so a track keeps recording after Navigate is closed (`out_nav.c:3-4`, `nos/svc_track.h:3-10`). Navigate only renders the track and sends start and stop.

On Stop the track is written to the SD card as GPX at `/sdcard/watch/tracks/NNNN.gpx` (`nos/svc_track.c:23`, `:37`, `:213`). Navigate has no route, no waypoints and no saved-track browser. Its only "navigation" is a bearing and distance back to the start of the current track.

Status is partial. Recording, the sky plot and the three pages are implemented. The gaps that matter in daily use are: a failed save cannot be discarded, an unplugged GPS module is never detected, the Control Centre GPS switch is overridden, and only speed follows the units setting. A track is also lost on reboot, crash or flat battery because it is held only in RAM until Stop. See Gaps.

## Features

- **Searching sky plot** (`sky_draw`, `out_nav.c:751-783`). Satellites are placed at their real azimuth and elevation from the GSV data (`:768-771`). Size has four SNR classes, 7, 10, 13 and 16 px at under 20, 30, 40 and 40+ dB-Hz (`:589`), tweened over 5 frames (`:591-598`). A satellite is white when used or at SNR 35 or more (`:773`). The label is SEARCHING, with a count line "N OF M" (used of in view), "M SATS", or "-" (`:565-567`, `:778-782`).
- **NOW page** (`page_now`, `:885-919`). Speed is the hero number, a morphing value updated once a second. It shows km/h or mph from `units_metric` (`:605`, `:891-895`). Latitude and longitude show 5 decimals with N/S and E/W (`:897-902`). The third row is "ALTITUDE m HDOP x.x" (`:911`). When the fix is lost it becomes "[ NO FIX ] m:ss AGO" (`:908`). The action row is at the bottom (`:918`, `:851-860`).
- **Header** (`header_kind`, `:720-737`). NAVIGATE when idle. "2D FIX" or "3D FIX  n SATS" when fixed (`:733`). While recording, "REC mm:ss  distance" with a red dot (`:724-731`).
- **Start track.** A tap on the START TRACK pill calls `svc_track_start()` (`:1093`). The pill is live only with a current fix. During the 30 s stale window it is drawn disabled (`A_START_DIS`, `:643-645`, `pill_colors` `:492-493`). A refused start toasts "CAN'T START" (`:1094-1097`).
- **Stop track.** Stop needs a 0.6 s hold (`HOLD_S`, `:55`; hold logic `:630-637`, `:1078-1081`). A short tap shows "HOLD TO STOP" and a limit haptic (`:1090`). Hold completion calls `stop_track()` (`:477-486`, `:634`).
- **Save result.** A three-second summary: "SAVED", "SAVE FAILED" or "TOO SHORT", with distance and duration (`summary_draw`, `:838-848`; `saved_state` mapping `:483`).
- **RETRY SAVE.** While a stopped track is pending, the pill reads RETRY SAVE and a tap calls `stop_track()` again (`:823`, `:643`, `:1091`). The service re-attempts the write (`nos/svc_track.c:273-310`).
- **TRACK page** (`page_track`, `:921-959`). The recorded path as dots, resampled at 9 px spacing, capped at 360 dots (`:370-371`, `MAXDOTS` `:51`). A start ring, a "you" dot that glides to each fix (`:945-950`), and a scale bar (`:953-957`). With no points it shows "No track / Start one on the NOW page" (`:926`).
- **BACK TO START page** (`page_back`, `:985-1006`). An arrow to the track start (`arrow_draw`, `:961-983`). The distance is in metres, or km past 1000 m (`:673-674`, `:1003`). The source line reads BY COMPASS, BY COURSE or NORTH UP (`:1004-1005`). Within 15 m of the start with a current fix it reads YOU ARE AT THE START (`:994-998`).
- **Module absent page** (`nav_draw_body`, `:1028-1033`). "Not connected / Add the GPS for speed and tracks / GPS MODULE".
- **Pager.** Three pages, horizontal drag with a spring and rubber-band at the ends (`pg_input`, `:75-109`; page pitch `PAGE_W` 466, `:50`).
- **Haptics.** Page change `FB_TICK` (`:116`), first fix `FB_SUCCESS` (`:555`), start `FB_TOGGLE_ON` (`:1101`), save result (`:485`, `:535`), refused start `FB_ERROR` (`:1095`).

Not in this app: route or bearing to any point other than the track start, waypoints, pause, average or max speed, pace, elevation gain, splits, a map, a track list, delete, rename, export, or phone upload. See Gaps.

## Reads

Track service (`nos/svc_track.h:32-40`):
- `svc_track_points()`: the PSRAM point array. Read by the view builder (`out_nav.c:256`, `:364`), the page checks (`:613`, `:656`, `:924`, `:988`).
- `svc_track_version()`: polled every tick to detect new points (`:467`, `:611`).
- `svc_track_active()` (`:474`, `:640`, `:643`, `:1074`), `svc_track_pending()` (`:643`, `:1074`), `svc_track_saving()` (`:466`, `:531`, `:643`).
- `svc_track_save_result()` (`:533`).
- `svc_track_distance()` (`:479`, `:500`), `svc_track_elapsed_ms()` (`:480`), `svc_track_label()` (`:726`, the "REC mm:ss" text).

GPS HAL (`nos/hal_ext.h:121-141`; driver `firmware/main/ext/ext_gps.c`):
- `hal_ext_present() & EXT_GPS` (`out_nav.c:537`; bit `nos/hal_ext.h:55`).
- `hal_gps_get()` once per tick (`out_nav.c:537`). Fields used: `valid`, `fix3d`, `lat`, `lon`, `alt_m`, `speed_mps`, `course_deg`, `hdop`, `sats_used`, `sats_view`, `age_ms`, `snr[]`, `el[]`, `az[]`, `nsnr`. The getter invalidates the fix when `age_ms` is over 3000 (`ext_gps.c:329-330`). Navigate also requires `age_ms <= 3000` and finite, in-range coordinates (`out_nav.c:547-548`).
- A read has a side effect: `hal_gps_get()` refreshes the driver's idle timer (`ext_gps.c:322`), so polling it keeps the stream alive.

Compass HAL (`hal_mag_get`, `out_nav.c:662`): `valid`, `calibrated`, `heading_deg`. It is called only when the track has points and a last fix exists (`:656-662`).

Preferences:
- `nos_prefs.units_metric`: speed only (`out_nav.c:605`, `:891`). Distances, altitude and the scale bar ignore it (Gaps).
- `nos_prefs.gps_auto` is not read here. Only `nos/sports.c:399` and the Settings and Control Centre code read it (`nos/apps/sys_control.c:180`, `:235`; `nos/apps/set_settings.c:1784`).

Engine (not data): `nos_t`, `nos_revealed()` (`out_nav.c:454`), `nos_enter_t()` (`:462`), `nos_subview` (`:1039`), `nos_keep_awake` (`:678`).

Diagnostics read: none.

## Writes

Track service:
- `svc_track_start()` (`out_nav.c:1093`). When allowed, clears the point count and distance and starts recording (`nos/svc_track.c:166-189`). Blocked while pending or saving, or while a module update or shutdown is pending (`svc_track.c:169`, `nos/runtime_admission.h:8-9`).
- `svc_track_stop()` (`out_nav.c:481`). Ends the track. Returns -1 when under two points, -2 when the save is queued, or the file number. The save runs in a task (`svc_track.c:273-310`). The only production caller is `stop_track()` in this file (confirmed by grep).
- `svc_track_discard()`: not called from production code (Gaps).

GPS HAL:
- `hal_gps_enable(1)` on enter (`out_nav.c:469`) and on every tick while the module is present (`:545`).
- `hal_gps_enable(0)` on leave, unless a track is active (`out_nav.c:474`). Off means the driver sends a UBX-RXM-PMREQ backup command and deletes the UART (`ext_gps.c:10-11`, `:73`, `:269-279`).
- `svc_track_tick` also calls `hal_gps_enable(1)` every recording frame (`nos/svc_track.c:106`).

Own RAM: the `N` state struct (`out_nav.c:149-209`) and the track-local-metre cache `L` (`:212`). `nav_enter` clears `N` except the two view dot buffers (`:452-470`). Buffers come from PSRAM: `L.xy` 32 KB (`:259`), and the two dot buffers, 2.9 KB each (`:326`, `:345`).

Diagnostics: `diag_action_request/context/result` in `DIAG_DOMAIN_NAVIGATION` on each page change (`out_nav.c:624-626`). These go to the RAM diag ring (`nos/diag_log.h:34`, `:40`). They are not user data.

Feedback and UI: `nos_feedback` (`:116`, `:485`, `:555`, `:1090`, `:1095`, `:1101`), `nos_toast("CAN'T START")` (`:1096`), `nos_keep_awake()` every tick (`:678`).

Navigate writes nothing to prefs, NVS or SD.

## Background behaviour

| State | What runs | Owner | Rate |
|---|---|---|---|
| Navigate on top, screen on | `nav_tick` (`out_nav.c:528`), then `svc_track_tick` through `svc_tick` | UI task (`app_main`, `firmware/main/nos_main.c:1899`; one thread per the Services hub note) | every display frame; `NOS_FPS` is 40 (`nos/render_config.h:8`) and the fps-cap pref can lower it |
| Navigate closed, screen on | `svc_tick` runs `svc_track_tick` first (`nos/svc.c:1308`), via `nos_bg_tick` (`nos/nos.c:2341`) | UI task | every frame; once-per-frame guard at `nos/svc_track.c:103` |
| Screen off (PWR_OFF) | `nos_bg_tick(1.0f)` once a second, `nos_frame++` | `firmware/main/nos_main.c:2091-2094` | 1 Hz |
| AOD and sensor modes | `frame_run` at 1 Hz, which also calls `nos_bg_tick` (`nos/nos.c:2299`, `:2341`) | `nos_main.c` AOD branch (`:2121`) | 1 Hz |
| GPS UART pump and NMEA parse | `gps_service` in the `ext` task (`firmware/main/ext/ext_sys.c:154`) | `ext` task, priority 0, 4096 B, PSRAM stack (`ext_sys.c:186`) | run: 25 ms at 115200 baud, else 80 ms (`ext_gps.c:284`); idle 500 ms (`:276`); absent 1000 ms |
| Compass | `mag_service` in the `ext` task | `ext_mag.c` | 20 Hz (`:203`) while `hal_mag_get` was called within 8 s (`:195`) |
| SD save of a track | `track_save` task, `xTaskCreate`, priority 1, stack 4096, unpinned | `nos/svc_track.c:299` | once per Stop |
| Track points | sampled in `svc_track_tick`, at most one fix per frame or per second with the screen off; a point is added when moved 5 m or more (`svc_track.c:145`) | UI task | fix-limited; the M10 output rate is not configured by the driver (`ext_gps.c` sends only the wake and PMREQ bytes) |

Notes on the table:
- A recording continues with Navigate closed, the screen off, or in AOD. The only stop is an explicit Stop in Navigate (no other production caller of `svc_track_stop`).
- The GPS stays on for a recording (`svc_track.c:106`). The driver's 8 s idle expiry (`ext_gps.c:269`) does not fire, because the track calls the getter every frame.
- Keep-awake: while Navigate is on top, `power_timeout_due` never fires (`nos/power_policy.h:18-19`, `firmware/main/nos_main.c:2080-2084`). The screen-off loop turns the display back on if Navigate is on top (`nos_main.c:2096`). So the display cannot sleep while Navigate is open, with or without a track. Navigate also keeps the GPS on for as long as it is open (`out_nav.c:545`).
- Power-sheet restart and deep sleep are refused while a track is active, pending or saving (`firmware/main/nos_main.c:745-746`, `:756-757`, toast "SAVE RECORDING FIRST").
- Re-entry: `nav_enter` returns early when `nos_revealed()` (`out_nav.c:454`), so a return from a covered screen keeps the state.

## Persistence

- Navigate itself: none. `N` is RAM and is cleared on enter (`out_nav.c:452-470`). The last fix is forgotten too (`have_last` is reset by the clear at `:457`). The BACK page therefore shows WAITING FOR FIX after re-entry, even though the track is still in RAM (`:1048-1052`).
- Track points: a PSRAM array of 4096 points at 32 B, 128 KB, allocated on the first start and never freed (`nos/svc_track.c:42`, `:174`; `nos/svc_track.h:15-16`). Lost on reboot, power-off, crash or flat battery. Nothing is checkpointed before Stop.
- After a successful save, the points stay in RAM until the next Start or a reboot (`svc_track.c:175`; the reset happens in `svc_track_start` and `svc_track_discard` only).
- GPX on Stop: `/sdcard/watch/tracks/NNNN.gpx` on the device, `build/nos_out/tracks/` in the simulator (`svc_track.c:23`, `:33`, `:37`). It is written to `NNNN.gpx.part` with `O_EXCL`, fsynced, then renamed (`:213-243`). The number is the first free one from 0001 to 9999 (`:212-221`). Each point carries its epoch time from `hal_epoch_seconds()` (`:160`, `:233`), so an unset clock gives untimed points.
- The write runs under `board_sd_asset_acquire()` and `hal_sports_sd_lock()` (`svc_track.c:252-255`). A failed write keeps `pending` set (`:291`). It clears only on success (`:261`) or discard (`:324-325`).
- Preferences: `units_metric` and `gps_auto` are ordinary prefs (`nos/prefs.c:153`, `:156`). They are saved through `prefs_save`, which writes `settings.<slot>.bin` (`nos/prefs_disk.c:32`). Navigate never writes them.

## Navigation

Opened from:
- Launcher tile "Navigate" (`nos/apps/registry.c:29`).
- The face REC chip while a track records. A tap calls `nos_open(sys_rec_screen(kind))`, and kind 1 (track) maps to `scr_nav` (`nos/apps/sys_home.c:171`, `:174`, `:502`, `:519`).
- The Control Centre REC chip (`nos/apps/sys_control.c:284`, `:352-357`).
- The launcher's last-app shortcut, for the most recently opened app (`nos/apps/sys_launcher.c:139`, `nos_last_app()` in `nos/apps/registry.c`).

Opens: nothing. `out_nav.c` has no `nos_open` call.

Exits: the engine's back gesture. Horizontal drags go to the pager (`SCR_HDRAG`, `nos/nos.h:60`) except from the left edge (`nos/nos.c:853`).

Pages: NOW (0), TRACK (1), BACK TO START (2). Spring-driven pager (`out_nav.c:68-119`, `:1034-1059`).

Input map:
- Tap on the action pill, page 0 only, when the module is present (`on_rec_button`, `out_nav.c:1062-1068`; tap handling `:1088-1106`).
- Hold on STOP for 0.6 s (`:1078-1081`).
- Horizontal drag: pager (`:1075`).
- Taps on TRACK and BACK do nothing. There is no handler.

Diagnostic hook: `out_nav_scroll_snapshot` (`out_nav.c:1115-1118`), read by `firmware/main/nos_main.c:1071`.

## Gaps

Ordered by severity. Each item has evidence and user impact.

1. **High. A failed or pending save cannot be discarded.** `svc_track_discard()` (`nos/svc_track.h:30`, `nos/svc_track.c:316-327`) has no production caller. Only `tests/svc_track_tracing_test.c:97` calls it. While pending, the only pill is RETRY SAVE (`out_nav.c:643`). `svc_track_start` refuses (`svc_track.c:169`). Power-sheet restart and deep sleep refuse with "SAVE RECORDING FIRST" (`firmware/main/nos_main.c:745-746`, `:756-757`). Impact: with no SD card, or after a failed write, the user cannot start another track. Restart and deep sleep stay blocked until a save succeeds.

2. **High. Recording is RAM-only until Stop.** Points live in PSRAM (`nos/svc_track.c:42`, `:174`). No write happens until `write_gpx` at Stop (`:273-310`). The Services hub reports the same gap (`docs/os-map/systems/Services hub.md:229`). Impact: a reboot, crash or flat battery mid-hike loses the whole track.

3. **Medium. An unplugged GPS module is never detected.** `EXT_GPS` is set on probe success (`firmware/main/ext/ext_gps.c:251`). Nothing in `firmware/main/ext` clears it. The speaker and mic bits are cleared at `ext_audio.c:347`, but not GPS. So `hal_gps_get()` keeps returning 1 (`ext_gps.c:319`), and `N.present` stays set (`out_nav.c:537`, `:542`). After unplug the page goes stale, then the sky SEARCHING forever. It never shows "Not connected" (`:1028-1030`). Re-probing happens only through `hal_gps_enable(1)` while absent (`ext_gps.c:294-295`). Impact: the user is asked to wait for a fix that cannot come.

4. **Medium. The Control Centre GPS switch is overridden while Navigate runs.** The switch writes `nos_prefs.gps_auto` and calls `hal_gps_enable(on)` (`nos/apps/sys_control.c:235`). Navigate calls `hal_gps_enable(1)` on enter and on every present tick (`out_nav.c:469`, `:545`). A recording re-enables it on every frame (`nos/svc_track.c:106`). Navigate does not read `gps_auto`. The pref only gates SP_GPS sports logging (`nos/sports.c:399`). Impact: switching GPS off does nothing while Navigate is open, and a recording ignores it.

5. **Medium. Compass arrow mixes magnetic heading with true bearing.** The target is a GPS true bearing (`out_nav.c:659`). The compass heading is magnetic (`nos/hal_ext.h:193` says "0 = magnetic north"; `firmware/main/ext/ext_mag.c:177-180`). No declination correction exists anywhere in `firmware/main/ext` (grep). Impact: the BACK arrow is off by the local magnetic declination whenever the compass is used (`out_nav.c:662-668`).

6. **Medium. Battery cost while Navigate is open.** `SCR_KEEPAWAKE` (`out_nav.c:1112`) blocks auto-sleep (`nos/power_policy.h:18-19`, `firmware/main/nos_main.c:2080-2084`). The screen-off loop turns the display back on (`nos_main.c:2096`). There is no timeout. The GPS also stays on for as long as the screen is open (`out_nav.c:545`), with no track running and the GPS switch off. Impact: forgetting Navigate open keeps the display and the GPS running. The effect on battery is not measured here.

7. **Medium. Missing features a modern GPS watch would offer.** There is no route or bearing to an arbitrary point (only to the track start). There are no waypoints and no "mark here". There is no pause or auto-pause. There is no average or max speed, pace, elevation gain or splits. The TRACK page is a bare dot plot with no basemap. There is no track history on the watch: only the most recent track, in RAM, until the next Start or a reboot. Earlier GPX files cannot be reached, deleted, renamed or exported from the watch, and phone upload of tracks is not implemented (`docs/os-map/systems/Services hub.md:17`). The summary shows distance and duration only (`out_nav.c:838-848`).

8. **Low. Only speed follows the units setting.** `fmt_dist` is always m or km (`out_nav.c:246-250`), used in the header (`:727`), the summary (`:844`) and the BACK page (`:1003`). Altitude is hard-coded "m" (`:911`). The scale bar is always m or km (`:953-954`). Impact: an imperial user sees mph next to metric distances.

9. **Low. The save outcome is silent if Navigate is closed during SAVING.** `save_wait` is resolved only in `nav_tick` (`out_nav.c:531-536`). `saved_state` is reset on enter (`:457`). A success leaves no trace on the watch. The file number is returned by the service (`nos/svc_track.c:245`) but dropped at `out_nav.c:534`, so the watch never shows which `NNNN.gpx` was written.

10. **Low. Long tracks lose resolution silently.** After 4096 points every other point is dropped (`nos/svc_track.c:149-157`), so spacing doubles on each halving. Nothing on the screen shows this.

11. **Low. Header comment and code disagree.** The file header says the NOW page shows "accuracy" (`out_nav.c:9`). The code shows HDOP (`:911`), a unitless dilution figure, not a distance accuracy.

12. **Low. Absent-module probe with no feedback.** While the module is absent, each entry into Navigate re-runs the two-round GPS probe, about 12.6 s (`firmware/main/ext/ext_gps.c:294-295`; `docs/os-map/systems/Hardware and drivers.md:121`). The page shows "Not connected" throughout, with no "searching for module" line.

13. **Low, unverified. GPS pin mapping has no B-board record.** GPIO7 RX and GPIO17 TX are defined only in `firmware/main/ext/ext_priv.h:39-40`, from the strap header comment (`nos/hal_ext.h`, top of file). `firmware/main/pins.h` (the 1.43-B branch, lines 36-67) has no GPS entries. The strap wiring was not verified on a board in this review.

14. **Audit cross-check (`docs/audits/2026-10-21/findings.csv`, rows APP-001 to APP-004, lines 12-15).** Current code does not reproduce three of the four claims, and the audit's line numbers do not match this tree. Re-check before acting on them.
   - APP-001, track frozen after a 500 m GPS gap: `svc_track_tick` recovers through the candidate anchor after at least 1 s of consistent fixes (`nos/svc_track.c:119-134`, `:139-144`).
   - APP-002, Stop removed after GPS loss: `svc_track_active()` takes precedence in the action kind (`out_nav.c:643`). The sky page draws the action row after the NOW fade (`:1045`), and `on_rec_button` does not require `show_now` (`:1064-1068`).
   - APP-003, dim returns after 30 s: `N.stale` stays true until a fix returns (`out_nav.c:559`), so dimming stays on (`:578-580`). YOU ARE AT THE START requires `N.fixed` (`:994`).
   - APP-004, Start discards a failed save: a pending save shows RETRY SAVE (`:643`), and `svc_track_start` refuses while pending (`svc_track.c:169`). Start does not discard. The missing discard path is Gap 1.

## Related systems

- [[Services hub]]: `svc_track` state, the tick order (`nos/svc.c:1303-1308`), start, stop and save, and the readers list.
- [[Sports and workouts]]: `SP_ROUTINE` on start and stop (`nos/svc_track.c:183`, `:286`), `SP_GPS` per NMEA sentence and `SP_MAG` from the compass.
- [[Power and battery]]: GPS backup and idle expiry, the keep-awake policy, and the screen-off wake on a keep-awake top screen.
- [[Storage and persistence]]: the GPX path, the SD lease and the sports SD lock, and the settings file.
- [[Hardware and drivers]]: the GPS UART and probe, the compass chip, and the `EXT_BOARD_OK` gate.
- [[Shell and faces]]: the REC chip on the face and in the Control Centre, and the launcher tile.
- [[Engine and scheduling]]: the frame loop, the screen tick dispatch, the 1 Hz screen-off background tick, and the ext task.
- [[Health and activity]]: no dependency. Navigate reads no step or heart data.
