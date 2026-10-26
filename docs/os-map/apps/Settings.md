---
title: "Settings"
id: app-settings
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Settings

Source: `nos/apps/set_settings.c` (2386 lines), 1.43-B build. Read-only source review. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`.

## Summary

Settings is the NOS preference list. The main screen is `scr_settings` (`nos/apps/set_settings.c:1880`). It holds 30 rows in six sections: Display, Sound & haptics, Connections, Time, Activity and System (`:426-488`). Three rows open sub-pages in the same file: a drum picker for screen timeout, time zone and step goal (`scr_set_pick`, `:2032`), a slider page for brightness, always-on brightness, volume and haptic strength (`scr_set_slide`, `:2262`), and a Set time page (`scr_set_time`, `:2381`). Other rows push or route to pages owned by other apps.

The screen holds no data of its own. Values are RAM fields of `nos_prefs` (`nos/prefs.h:7-46`) or live reads from HAL and service getters, which are refreshed in `update_values()` (`:568-614`). Writes go through `prefs_save()`, which only marks the struct dirty (`nos/prefs.c:260-265`). The SD write happens later in the prefs owner. The screen has no task, timer or ISR of its own.

Hardware: opening the screen needs only the display and the MCU. Each row needs the hardware it describes. Brightness, timeout, time and most toggles need nothing more. The Auto brightness row needs the APDS light strap (`firmware/main/ext/ext_light.c:113`). Storage needs the SD card. Volume and haptic rows are gated on strap presence bits, but on B those bits are set without a probe (see Gaps).

Status: partial. Most rows drive live consumers. The gaps are a dead GPS row, speaker and haptic rows that look live on B without a probe, Reset that also resets time zone and watch face, a restart that can be refused after the screen has said RESTARTING, and Set time that cannot set a date.

Related: [[Services hub]], [[Storage and persistence]], [[Hardware and drivers]], [[Power and battery]], [[Connectivity and voice]], [[Shell and faces]], [[Engine and scheduling]], [[Health and activity]], [[Sports and workouts]], [[Settings and developer]].

## Features

Rows, in list order (`nos/apps/set_settings.c:451-488`):

- **Display**
  - Brightness: slider page, 5-100 %. The "Auto N%" row appears only with a light strap (`:2172-2183`, `:2063-2068`).
  - Theme: Dark/Light flip (`:1744`).
  - Always-on display: toggle (`tog_pref` `:526`).
  - Always-on brightness: slider, 1-100 %. The whole page shows that level while it is open (`:2125`, `:2132`).
  - Screen timeout: picker over 5, 10, 15, 30 and 60 s (`k_timeout`, `:391`; picker `:1905`).
  - Raise to wake: toggle (`:527`).
  - Learn reading angle: starts a wake-service learn for the chosen side (`:1834-1849`). The result toast shows after the learn ends (`:688-695`). The learn times out after 10 s (`core/wake_service.c:94`).
  - Knock to lock: toggle (`:528`).
  - Watch face: opens the face picker (`:1852`).
- **Sound & haptics**
  - UI sounds: toggle (`:529`).
  - Volume: slider, 0-100 % (`:2085-2101`).
  - Haptics: toggle (`:530`).
  - Haptic strength: slider, 0-100 % (`:2096`).
  - Do not disturb: toggle (`:531`).
- **Connections**
  - Bluetooth: status only, "Connected" or "Not connected" from the phone link (`:417`, `:586`). Opens the connectivity page (`:1851`).
  - Wi-Fi: "Off", "Joining", "Connected" or "Not connected" (`:419-423`, `:587`). Opens the connectivity page (`:1851`).
  - Cellular: operator name when registered, otherwise On, Off or "No module" (`:589-592`). Opens the Cellular app (`:1853`).
  - GPS auto: always shows "N/A" (`:1411-1413`). Tapping toasts "GPS AUTO UNAVAILABLE" and records a rejected action (`:1783-1789`).
- **Time**
  - 24-hour clock: toggle (`:534`).
  - Time zone: picker over the fixed UTC offset table (`k_tz`, `:386-389`), with a local-time preview (`:1909-1921`).
  - Set time: hour and minute drums plus a SET button (`:2272-2381`).
- **Activity**
  - Step goal: picker from 2,000 to 30,000 steps in 500-step detents (`:1931`, clamp `:1952`).
  - Wrist: Left/Right flip (`:1743`, `prefs_set_wrist(..., PREFS_WRIST_SOURCE_MANUAL)`).
  - Units: Metric/Imperial flip (`:1745`).
- **System**
  - Modules: "n/16" online count (`:600`, `set_modules.c:249-260`). Opens Modules (`:1855`).
  - Battery: opens the Battery page (`:1856`).
  - Storage: free GB, or "No card" (`:607-611`). Opens the SD page of Modules (`:1857`, `set_modules.c:631`).
  - About: version string (`:612`, `:1858`).
  - Restart: arm, then restart (`:1806-1828`).
  - Reset settings: arm, then reset all preferences (`:1754-1774`).

Interaction rules:

- Restart and Reset arm on the first tap and act on a second tap within 3 s (`ARM_S`, `:68`, `:680`).
- Toggles and arm pills animate on whole-frame timings described in the file header (`:15-26`). These are presentation only.
- The list scrolls on a 30 px unit grid, starting at y=96 (`UNIT`/`LTOP`, `:490-491`; list init `:626`).
- Sliders apply on every change. Pickers commit on every detent crossing, including during a drag. Neither has a confirm step.

## Reads

Services:

- `svc_sys()` (`nos/svc.c:986`, struct `nos/svc.h:179-184`): `battery_pct`, `charging`, `phone_connected`. Read by `update_values` (`nos/apps/set_settings.c:602-604`) and `phone_linked` (`:417`).
- `wake_service_get()` (`core/wake_service.c:87-88`): wrist, learning state and learn result. Read every tick (`set_settings.c:580-582`, `:687`, `:689`).
- `nos_face_count` and `nos_faces[]` (`nos/faces/face.h:38-39`): watch face name (`set_settings.c:574-576`).
- `set_modules_online()` (`nos/apps/set_modules.c:249-260`): module count. Called on enter and about once a second (`set_settings.c:654`, `:679`). Internally it reads the IMU (`hal_imu_read`, a cached snapshot at `firmware/main/board.c:388-395`), storage and power.

Preference fields (RAM copy of `nos_prefs`):

- `theme` (`:597`), `wrist_left` (`:446`, `:598`), `units_metric` (`:447`, `:599`), `sleep_s` (`:447`, `:573`), `face` (`:575`).
- `brightness` and `auto_bright` (`:571-572`), `aod_bright` (`:577`), `volume` (`:584`), `haptic_strength` (`:585`).
- `wifi` (`:421`), `tz_min` (`:448`, `:593`), `step_goal_100` (`:448`, `:596`).
- Toggles `aod`, `raise_wake`, `knock_lock`, `sound`, `haptics`, `dnd`, `hour24` (`tog_pref`, `:523-537`, loaded by `settings_sync_toggles` `:632-638`).
- `auto_bright` and `aod_bright` again in the slider page (`:2121`, `:2243`).
- `wrist_revision` (`:1746`).

HAL and driver getters:

- `hal_ext_present()` (`firmware/main/ext/ext_sys.c:197-199`): strap presence bits. Read on enter, on tick and in prewarm (`set_settings.c:651`, `:677`, `:1283`), and per row in `tog_absent` (`:539-544`) and `slide_hw` (`:2063-2067`).
- `hal_storage_get()` (`firmware/main/board.c:534-540`): cached mounted flag and free bytes. Polled every 2 s (`set_settings.c:652`, `:678`).
- `hal_lte_get()` (`firmware/main/ext/ext_lte.c:463-470`): cached modem state. Called only when the LTE bit is set (`set_settings.c:589`). It has a side effect (see Background behaviour).
- `hal_light_get()` (`firmware/main/ext/ext_light.c:236-246`): cached lux. Read every slider tick (`set_settings.c:2149`).
- `hal_get_datetime()` (`firmware/main/hal_esp32.c:116-128`, localtime): Set time values (`:594-595`, `:2291`, `:2358`).
- `hal_epoch_seconds()` (`firmware/main/hal_esp32.c:148-151`): time zone preview only (`:1915`).
- `hal_battery_percent()` (`firmware/main/hal_esp32.c:392`): fallback when `svc_sys` has no reading (`:603`).
- `hal_phone_connected()` (`firmware/main/hal_esp32.c:1159`) and `hal_status(HAL_FEAT_WIFI)` (`:421`, `firmware/main/hal_esp32.c:897`): Bluetooth and Wi-Fi rows.
- `prefs_loading()` (`nos/prefs.c:283-285`): gates Reset.

Shared state written by other screens: Control Centre tiles write the same toggles (`nos/apps/sys_control.c:228-235`), and voice chat writes `volume` and `wifi` (`firmware/main/voice_chat.c:150`, `:583`). Settings re-syncs toggles on each entry (`set_settings.c:650`, `:1766`).

## Writes

Preference fields (followed by `prefs_save()` unless noted):

- Toggles through `activate` and `tog_pref`: `aod`, `raise_wake`, `knock_lock`, `sound`, `haptics`, `dnd`, `hour24` (`set_settings.c:1782-1803`, save at `:1801`).
- Theme (`:1744`) and Units (`:1745`) flips, with `prefs_save()` in `flip` (`:1748`).
- Wrist flip through `prefs_set_wrist` (`:1743`). That function saves on its own (`nos/prefs.c:318-327`, save at `:326`).
- Brightness, always-on brightness, volume and haptic strength through `slide_apply` (`:2085-2101`, save at `:2088`).
- Auto brightness toggle: `nos_prefs.auto_bright`, then save (`:2242-2249`).
- Screen timeout, time zone and step goal through `pick_commit` (`:1927-1939`). It runs on every detent crossing during a drag (`:1965-1967`) and on taps in `pick_input` (`:1995-2031`).
- Reset settings: `prefs_defaults()` clears the whole struct, then sets defaults (`nos/prefs.c:134-163`). Then `prefs_save()` and `prefs_flush()` (`set_settings.c:1763-1765`).
- Restart: `prefs_save()` and `prefs_flush()` (`:1818-1819`).

Hardware and driver calls:

- Brightness: `hal_set_backlight(v*255/100)` (`:2090`), which calls `display_set_brightness` (`firmware/main/hal_esp32.c:437-440`). The frame loop then reasserts its own level every pass (`firmware/main/nos_main.c:2226`, `user_level()` `:648-655`).
- Always-on preview: `nos_brightness_override()` (`:2091`, `:2125`, `:2132`), which is defined at `nos/nos.c:576-581`.
- Volume: `hal_audio_volume()` (`:2093`, `:1768`), defined at `firmware/main/ext/ext_audio.c:374`.
- Volume detent click: `hal_sfx(SFX_TICK)` (`:2094`), which does not check the UI sounds pref.
- Haptic strength: `hal_haptic_set_strength()` (`:2096`, `:1769`), defined at `firmware/main/ext/ext_haptic.c:126`. Click: `hal_haptic()` (`:2097`).
- Toggle and tap feedback: `nos_feedback()`, which honours the haptics and sound prefs (`nos/nos.c:557-573`).
- Learn: `wake_service_set("wrist", v)` (`:1840`), then `wake_service_learn()` (`:1845`). See Persistence for where this lands.
- Set time: `hal_set_datetime_checked()` (`:2361`). It refuses any date outside 2000-2099 (`board_datetime_valid`, `firmware/main/hal_esp32.c:156`, `firmware/main/board_protocol.c:19-25`). Otherwise it applies the time zone, `mktime`, `settimeofday` and `board_clock_changed()` (`firmware/main/hal_esp32.c:154-172`). The RTC is marked dirty for a later write (`firmware/main/board.c:747-754`).
- Restart: `nos_host_power(1)` (`:686`). See Gaps for refusal behaviour.
- Screen changes: `nos_push()` to the sub-pages and other apps (`:1846-1858`, `:1946`, `:2083`). `nos_pop()` after a successful Set time (`:2366`).

Diagnostics: every change calls `diag_action_request`, `diag_action_context` and `diag_action_result` in domain CONTROL with target `0x5000 + id` (`set_settings.c:438-443`, `:1756`, `:1770`, `:1796`, `:1937`, `:2107`, `:2248`, `:2362`). Records are queued in RAM and appended to SD (`nos/diag_log.c:229-243`, `:463-503`). Each picker detent therefore produces three records.

Nothing else is written: Wi-Fi, Bluetooth and Cellular are not toggled here. Each of those rows routes to its owning page.

## Background behaviour

- Settings has no task, timer or ISR. Its `settings_tick` (`set_settings.c:674-698`) is driven by the engine only while the screen is on top or in a transition (`nos/nos.c:2104-2108`). Settings' own callbacks do not run with the screen closed. Other subsystems that Settings writes into (wake learn, prefs flush, diagnostics) do run in the background, as described below.
- `set_settings_prewarm()` (`:1279-1287`) runs only while Home is on top, not transitioning, and `nos_mblur` is set. The default is 2 unless `NOS_RENDER_SHARP` is set (`nos/nos.c:34`). Home runs it on odd frames while the face is not interactive, rotated with the launcher and notification prewarmers (`nos/apps/sys_home.c:638-647`). It refreshes `update_values()`, including the LTE getter, and advances the row bake (`rows_work`, `:1195-1229`).
- LTE idle lease: `hal_lte_get()` writes `L->last_get` (`firmware/main/ext/ext_lte.c:466-467`). The modem turns itself off once `NOS_EXT_IDLE_MS` (8000 ms, `nos/hal_ext.h:35`) passes without a read (`ext_lte.c:302`). Settings and Home refresh that timestamp every frame and every prewarm while they are shown. So an enabled modem does not idle off while the screen is on. This is from code reading and has not been measured.
- Learn: `wake_service_tick` ends a learn after 10 s (`core/wake_service.c:94`). It runs from the main loop (`firmware/main/nos_main.c:2038`), so learning continues with Settings closed. The result toast shows only while Settings is top (`set_settings.c:688-695`).
- Restart timer: `activate` sets `S.restart_at` 0.8 s after the RESTARTING toast (`:1825-1826`). `settings_tick` fires it (`:686`), which calls `nos_host_power(1)`. `settings_enter` clears it (`:660`).
- Preference flush: `prefs_tick()` is called from the main loop at `firmware/main/nos_main.c:2098` and `:2110`, both under `if(mutate)` in the power-state branches, and at `:2164`. The last one runs only when `mutate`, no voice chat, no touch, no transition, no injected input and no frame capture are active (`:2162`). This is where Settings' writes reach the SD card. Settings does not flush by itself, except on Restart and Reset.
- Diagnostics flush: queued action records reach SD through `diag_log_service()` from the `sports_sd` writer task (`nos/sports.c:282-289`, task created at `:323` with priority 1).
- Clock: a Set time write marks the RTC dirty (`firmware/main/board.c:747-754`). The write happens later on the board side.
- Settings values read by the power policy: `sleep_s` (`firmware/main/nos_main.c:2084`).
- Loop context: the main task is pinned to CPU0 (`firmware/sdkconfig:1707-1710`). `app_main` (`firmware/main/nos_main.c:1899`) runs the loop that drives Settings' tick and the prefs flush.

## Persistence

- Live state: the `nos_prefs` RAM struct, 52 bytes, with static asserts on its layout (`nos/prefs.h:7-46`, `nos/prefs.c:14-16`).
- Dirty and debounce: `prefs_save()` sets the dirty flag (`nos/prefs.c:260-265`). `prefs_tick()` flushes 2 s after the last edit, or 10 s after the first unsaved edit. After a failed write it retries every 5 s (`nos/prefs.c:366-384`).
- Explicit flush: `prefs_flush()` (`nos/prefs.c:331-364`) skips the debounce. Restart and Reset use it. It returns 0 while a restore is in progress or the card load is unresolved (`nos/prefs.c:333-344`).
- SD (authoritative on B): `hal_store_save_checked("nosprefs")` writes through `prefs_disk_save("/sdcard/watch")` (`firmware/main/hal_esp32.c:228-252`). The write alternates between two banks, `/sdcard/watch/settings.0.bin` and `settings.1.bin`, with a checksum and an fsync (`nos/prefs_disk.c:11`, `:32`, `:99`). A save is refused while the boot read is still pending (`hal_esp32.c:237`).
- NVS mirror: namespace `wos`, key `nosprefs` (`firmware/main/hal_esp32.c:181`, `:277`). The mirror is written whenever the card is allowed to be written (`hal_esp32.c:250`), even if the SD write itself failed. If the SD asset cannot be leased, the save reports failure and the RAM copy stays dirty.
- Field-level edits recorded in a mask are checkpointed to NVS key `prefs_overlay` (`nos/prefs.c:118-130`, `firmware/main/hal_esp32.c:203-212`).
- Boot read: SD first (`firmware/main/hal_esp32.c:258-282`). NVS is read when the SD bank is missing, or when the SD asset cannot be acquired, in which case the load is marked pending. An I/O or corrupt result returns without touching NVS, so unreadable settings are never silently replaced.
- Wake calibration (`wake_cfg`): NVS, not SD. On B the key is not `nosprefs` or `wifi_cred`, so it takes the NVS branch (`firmware/main/hal_esp32.c:236`, `core/wake_service.c:63`).
- Time: the Set time value goes to the system clock and the RTC, not to `nos_prefs`. The time zone is `tz_min` in `nos_prefs`.
- Diagnostics records: RAM queue, then `/sdcard/watch/diagnostics/diag-NN.bin` (`nos/diag_log.c:24`, `:463-503`).
- Reset settings clears all `nos_prefs` fields, including: time zone (to +05:30, `nos/prefs.c:154`), watch face (`:148`), Wi-Fi intent (`:150`), units, 24-hour clock, wrist side, and the sports and wrist-calibration prefs (`nos/prefs.h:36-38`). Those prefs are cleared because the whole struct is zeroed first (`nos/prefs.c:136`). Reset does not touch `wake_cfg` (NVS), the sports stores `sportsfood` and `sportsmarks` (`nos/sports.c:310-313`), or Wi-Fi credentials (`wifi_cred`, `firmware/main/hal_esp32.c:2004`).

## Navigation

Opened from:

- The launcher grid: "Settings" in `nos/apps/registry.c:36`.
- The Control Centre settings tile (`nos/apps/sys_control.c:42`, `:613-617`).

Opens (pushed with `TX_PUSH`):

- Picker (`scr_set_pick`) for screen timeout, time zone and step goal (`:1946`).
- Slider (`scr_set_slide`) for brightness, always-on brightness, volume and haptic strength (`:2083`).
- Set time (`scr_set_time`) (`:1854`).
- Watch face picker: `scr_faces` (`:1852`, `nos/apps/sys_faces.c:443`).
- Cellular: `scr_cell` (`:1853`, `nos/apps/con_cell.c:419`).
- Modules: `scr_modules` (`:1855`, `nos/apps/set_modules.c:665`).
- Battery: `scr_battery` (`:1856`, `nos/apps/set_battery.c:94`).
- About: `scr_about` (`:1858`, `nos/apps/set_about.c:345`).
- Storage: the Modules SD page (`:1857`, `set_modules_open_sd`, `nos/apps/set_modules.c:631`).
- Bluetooth and Wi-Fi: the connectivity page through `set_connectivity_open(bt)` (`:1851`, `:2266-2270`, `nos/apps/set_connectivity.c:144`).

Engine-level: Restart goes through `nos_host_power()` (`firmware/main/nos_main.c:751-762`). Mode 1 ends in `esp_restart()` (`:809`). The slider page uses `SCR_HDRAG` (`:2262`), so horizontal drags stay with it.

## Gaps

Each gap lists severity and evidence. Evidence is `path:line`.

1. **Reset settings resets more than the user expects, and the confirm pill says nothing about it (high).** Evidence: `nos/prefs.c:134-163` (the struct is zeroed, then time zone is set to 330 at `:154`, face to 0 at `:148`, Wi-Fi to off at `:150`); `nos/apps/set_settings.c:1754-1774`; the pill label "TAP AGAIN TO RESET" at `:1375`; the sports body and profile prefs at `nos/prefs.h:36-37`. Impact: the time zone silently moves to IST, the watch face and Wi-Fi intent change, and the sports body and profile prefs (`sports_mass_kg`, `sports_dominant`, `sports_racket` and the rest) are cleared. The UI says only "TAP AGAIN TO RESET".

2. **A restart can be refused after the screen has already said RESTARTING (high).** Evidence: `set_settings.c:1825-1826` (toast "RESTARTING", then `restart_at`), `:686` (calls `nos_host_power(1)`). `firmware/main/nos_main.c:751` declares `nos_host_power` as `void`, so Settings cannot see a refusal. Inside it, `shutdown_guard()` (`:731-746`) blocks for OTA, voice chat (which it cancels), IMU record/file/stream, audio recording, a running or unsaved workout, or storage/track saving. The refusal then shows "UPDATE ACTIVE", "STOP IMU FIRST" or "SAVE RECORDING FIRST" (`:756-757`) and returns without restarting. On success it shows "WAITING FOR SHUTDOWN" (`:761`). Impact: the user sees "RESTARTING", then one of the refusal messages, and the device stays up.

3. **Speaker and haptic rows look live on B without any probe (medium).** Evidence: `firmware/main/ext/ext_priv.h:27-29` sets `EXT_BOARD_OK` to 0 only for the 1.43C. `EXT_SPEAKER` is set without a probe ("cannot be probed; default on", `firmware/main/ext/ext_audio.c:343`) and cleared only if the audio task fails (`:347`). `EXT_HAPTIC` is set when the haptic driver starts (`firmware/main/ext/ext_haptic.c:107-109`). So "UI sounds", "Haptics", "Volume" and "Haptic strength" never show "NO SPEAKER" or "NO HAPTIC" (`set_settings.c:539-544`, `:2063-2067`, `:2186-2188`). `CLAUDE.md` records that B has no built-in audio and that any external strap mapping is unverified. Impact: a user with no strap gets no warning that Volume does nothing.

4. **The saved volume and haptic strength never reach the drivers at boot (medium).** Evidence: the only callers of `hal_audio_volume` and `hal_haptic_set_strength` are this screen and Diagnostics (`set_settings.c:1768-1769`, `:2093-2096`; `nos/apps/set_diag.c:634-644`). The drivers start at 70 (`firmware/main/ext/ext_audio.c:337`) and 80 (`firmware/main/ext/ext_haptic.c:43`). `voice_audio_levels` is a no-op off the C board (`firmware/main/voice_audio.c:115`). Impact: the screen shows the saved value (defaults 60 and 70, `nos/prefs.c:141`, `:139`), but the hardware runs at the driver default until a slider is touched.

5. **Set time cannot set a date, and it fails when the clock was never synced (medium).** Evidence: `set_settings.c:2359-2370` takes the current year from the clock and changes only hour and minute. `firmware/main/hal_esp32.c:156` and `firmware/main/board_protocol.c:19-25` refuse any year outside 2000-2099. There is no date control anywhere in the list (`:451-488`). Impact: with no valid clock, the user gets "DATE NOT SET: SYNC PHONE" and cannot set a time manually.

6. **Cellular modem kept on by a side effect of reading status (medium, power).** Evidence: `set_settings.c:589` calls `hal_lte_get()` through `update_values`, which runs every tick while open and in the Home prewarm (`:1279-1287`). `firmware/main/ext/ext_lte.c:466-467` refreshes the idle timer and `:302` turns the modem off after 8 s without a read. Impact: while Home or Settings is shown, an enabled modem does not idle off. Measure before relying on this.

7. **GPS auto is a dead row (low).** Evidence: the row is fixed at "N/A" (`set_settings.c:1411-1413`), and tapping toasts and records a rejected action (`:1783-1789`). The same pref still drives sports GPS output (`nos/sports.c:399`) and the Control Centre GPS tile (`nos/apps/sys_control.c:180`, `:235`). Impact: a visible control that never works, next to a live one.

8. **Volume detent click ignores the UI sounds pref (low).** Evidence: `set_settings.c:2092-2094` calls `hal_sfx(SFX_TICK)` with no check of `nos_prefs.sound`. The same page shows "UI SOUNDS OFF" (`:2187`). Impact: dragging the volume with UI sounds off still clicks.

9. **The brightness slider uses a different curve from the frame loop (low).** Evidence: the slider writes `v*255/100` (`set_settings.c:2090`). The frame loop computes `6 + 2.49*pct` (clamped 4-255), times the ambient factor `s_auto_k` when Auto is on (`firmware/main/nos_main.c:648-657`), and reapplies it every pass (`:2226`). Impact: the immediate write is overwritten within a frame, and the displayed percentage does not match the panel level linearly.

10. **The two phone time paths disagree on time zone (low).** Evidence: the Gadgetbridge path saves the phone's offset into `nos_prefs.tz_min` (`nos/svc.c:1005`). The BLE TIME characteristic only calls `apply_tz`, which changes `g_tz_offset_min` (`firmware/main/hal_esp32.c:881-883`). `calendar_preferences()` then resets `g_tz_offset_min` to the saved `nos_prefs.tz_min` on the next loop pass (`firmware/main/nos_main.c:962-965`, called from `:1987`). Impact: a BLE time sync does not stick. The Time zone row keeps showing the saved value, while the Gadgetbridge path does persist.

11. **Picker commits on every detent during a drag, with three diagnostic records each (low).** Evidence: `set_settings.c:1965-1967`, `:1927-1939`, `nos/diag_log.c:229-243`. Impact: a long drag writes many records and sets the prefs dirty repeatedly. There is no confirm or cancel step.

12. **The step goal row and its picker can disagree (low).** Evidence: the row shows `step_goal_100 * 100` (`set_settings.c:596`). The picker's starting index is `(goal*100 - 2000)/500`, rounded and clamped (`:1952`), so the picker can open on a different detent than the row's value. Example: 8,100 steps shows as 8,100 in the row, but the picker opens on 8,000 until the user moves it (`:1931`).

13. **Learn result is shown only while Settings is open (low).** Evidence: `set_settings.c:688-695` polls the result only from `settings_tick`. Learning itself continues in `wake_service_tick` (`core/wake_service.c:94`; `firmware/main/nos_main.c:2038`). Impact: a learn finished with Settings closed gives no feedback.

14. **Expected settings are absent from the list (low).** Evidence: the complete row table is `nos/apps/set_settings.c:451-488`. There is no date entry, no time-sync status (only "TIME NOT SYNCED" in the zone preview, `:1916`), no passcode (lock is knock-only, `nos/nos.c:652`), no "never" timeout (`:391`), no per-event sound or haptic control, no text-size or reduced-motion option, no language, and no software-update entry. Bluetooth here is status only; its on/off control lives in the Control Centre (`nos/apps/sys_control.c:229`). Impact: users coming from a modern watch will not find these.

15. **Doc drift (low).** Evidence and corrections:
   - `docs/os-map/systems/Settings and developer.md` says Reset leaves the sports blobs alone. The sports fields in `nos_prefs` are cleared (`nos/prefs.c:136`, `nos/prefs.h:36-37`). Only the separate stores (`nos/sports.c:310-313`) survive.
   - The same note says diagnostics results are not stored. This Settings screen's action records do reach SD (`nos/diag_log.c:463-503`).
   - The systems note's description of B audio is not backed by a probe for the speaker and haptic rows (see Gap 3, `firmware/main/ext/ext_audio.c:343`, `firmware/main/ext/ext_haptic.c:107-109`).
