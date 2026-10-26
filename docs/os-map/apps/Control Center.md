---
title: "Control Center"
id: sys-control-center
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Control Center

## Summary

Control Center is the NOS system sheet that drops down from the Home face. The source is `nos/apps/sys_control.c` (752 lines), registered as `scr_control` (`nos/apps/sys_control.c:752`, declared `nos/apps/nos_apps.h:13`). It is not a launcher app. It is opened by a downward drag from Home page 0 (`nos/apps/sys_home.c:1088-1090`), or by the serial command `open control` (`firmware/main/nos_main.c:1558`, name lookup `:924-935`).

It owns no state. Each tile is a live view of one field in `nos_prefs` (RAM copy, checkpointed to SD and NVS by the prefs layer). The brightness bar writes `nos_prefs.brightness` and the backlight. The sheet has no task, timer or poller. Its `c_tick` runs only while it is the top screen or during a screen transition (`nos/nos.c:2101-2110`).

Two pages. Page 0 holds a header (clock, battery or REC chip, transient note), eleven tiles and a brightness pill. Page 1 ("QUICK TOOLS") holds four links to other screens.

Status is **partial**. The toggles and brightness write real preferences that reach the radios and the power and feedback code. Several tiles show a state the hardware does not follow: AOD does nothing under default prefs, Sound and Haptics report present on a bare B, LTE and GPS are cosmetic, and one tool label is wrong. Alarm, timer and call rings ignore the Sound, Haptics and DND settings. Source-only card; not checked on a watch.

Related: [[Shell and faces]] (Home pull-down and screen engine), [[Services hub]] (`svc_sys`), [[Power and battery]] (battery and idle policy), [[Storage and persistence]] (prefs checkpoint), [[Hardware and drivers]] (strap modules), [[Connectivity and voice]] (Wi-Fi, BLE, LTE), [[Engine and scheduling]] (main loop and tick order), [[Sports and workouts]] (GPS gate, REC chip).

## Features

Header (`nos/apps/sys_control.c:428-459`)

- Clock as `HH:MM` from the system clock. 12 h when `nos_prefs.hour24` is 0 (`nos/ui.c:901-905`). No AM/PM marker is shown.
- Battery percent. It is bright when `svc_sys().charging` is set and dim otherwise (`:453-458`). A tap on the header (not on the REC chip) opens the BATTERY screen (`:718-719`).
- REC chip (`:444-449`, `:586-587`). It replaces clock and battery while any recorder runs. `sys_rec_state()` picks one label in this order (`nos/apps/sys_home.c:150-172`): voice memo `REC mm:ss` (kind 2), IMU record `IMU mm:ss` (kind 3), workout `SPORT` or `PAUSE` (kind 4), GPS track (kind 1). A tap lights the chip for 3 frames, then opens the owning screen (`:712-717`, `:355-359`, `nos/apps/sys_home.c:174`).
- Transient note under the clock (`:201-218`, `:431-438`). After a change it shows for about 1.6 s, e.g. `WI-FI OFF`, `BLUETOOTH ON`, `BRIGHTNESS 60%`, `NO LTE`. A new subject crossfades. The note is kept up while the bar is dragged (`:151`, `:343`).

Tiles (`:42-46`, `:85-97`, `:161-166`). Three rows at y 124, 220 and 316, 96 px pitch, r 40 discs.

- Toggles (nine): Wi-Fi, Bluetooth, DND, AOD, Sound, Haptics, LTE, GPS, Raise. On is a white disc with black ink. Off is grey with white ink. Each tap flips the pref, calls `prefs_save()`, plays haptic and sound feedback, and morphs the fill in three steps (`:220-246`).
- Torch (opens a screen): tapping opens `scr_torch` from the tile disc (`:613-615`). A long press opens it too (`:700-707`). It is not a light toggle.
- Settings (opens a screen): opens `scr_settings` (`:613-616`).
- Absent strap module: tile is drawn grey with a dim icon. A tap writes a rejected diag record, plays the error haptic and shows `NO <MODULE>` (`:602-611`). Only Sound (`EXT_SPEAKER`), Haptics (`EXT_HAPTIC`), LTE (`EXT_LTE`) and GPS (`EXT_GPS`) can be absent (`:85-97`, `:168`).

Brightness pill (`:47-62`, `:464-471`, `:722-741`)

- A 257 x 50 pill with ten 15 x 18 segments. Drag is 1:1 from the grab point (`:685`). A tap jumps to the segment with a 4-9 frame tween (`:730-736`). Feedback ticks at each 10 % step and a limit tick at the ends (`:687`, `:737`).
- Floor is 5 % (`:251`). Value is written live to `nos_prefs.brightness` and the backlight (`:252-253`).
- Entry: the fill sweeps in over 15-27 frames when the sheet is not pulled in by the finger (`:291-295`).

Quick tools, page 1 (`:494-504`, `:654-660`)

- Swipe left to reach it, then tap a row. Rows: FILE QUICK REPORT (`scr_quick_report`), ENERGY (`scr_energy`), BATTERY HISTORY (`scr_battery_history`), LOGS / WRIST TUNING (`scr_wrist_tuning`). The last one is mislabelled (Gaps). Two page dots at y 443 (`:512-513`).

Defaults that matter (`nos/prefs.c:134-160`): haptics 1, sound 1, brightness 70, auto_bright 1, aod 0, raise_wake 1, wifi 0, ble 1, lte 0, gps_auto 1, sensor_idle 1, dnd 0 (zeroed by `memset`).

## Reads

Services (`nos/svc.h`, implemented in `nos/svc.c`)

- `svc_sys()` gives `battery_pct` and `charging` (`sys_control.c:344-346`, `:532-534`). `charging` is `external_power == 1` (`nos/svc.c:992`). `svc_sys` refreshes once a second from `svc_tick` (`nos/svc.c:1303-1322`, `sys_tick` at `:1317`).
- `svc_sys()` also has `wifi_up` and `phone_connected` (`nos/svc.c:995-996`). The sheet does not read either.
- REC chip (`nos/apps/sys_home.c:150-172`): `svc_memo_active()`, `svc_memo_elapsed_ms()`, `hal_imu_record_get()`, `workout_get()`, `svc_track_active()`, `svc_track_label()`.

Preferences (`nos_prefs`, keys in `nos/prefs.h:7-46`)

- Tile state, read every tick through `state()` (`sys_control.c:170-184`): `wifi`, `ble`, `dnd`, `aod`, `sound`, `haptics`, `lte`, `gps_auto`, `raise_wake`. `c_tick` re-reads them and re-morphs any tile changed elsewhere (`:332-335`).
- `brightness` (`:252`, `:283`, `:530`).
- `hour24` through `ui_fmt_time` (`nos/ui.c:903`).

HAL

- `hal_battery_percent()`, a fallback only (`sys_control.c:345`, `:533`). It reads the same snapshot as `svc_sys` (`firmware/main/hal_esp32.c:392`), so the fallback adds nothing.
- `hal_get_datetime()` (`sys_control.c:348`, `:535`). It reads the system clock through `gettimeofday` and `localtime_r` (`firmware/main/hal_esp32.c:116-130`, `:142-146`).
- `hal_ext_present()` (`sys_control.c:168`; `firmware/main/ext/ext_sys.c:197-199`).

Engine and UI helpers (inputs to drawing and navigation, not data): `nos_touch_travel`, `nos_revealed`, `nos_entered_by_finger`, `nos_enter_t`, `nos_depth`, `nos_top`, `nos_transitioning`, `nos_subview`, `nos_ink_dim`, `pblur_*` (blur slot 6, `nos/pblur.c:10`), `ui_module_short` (`sys_control.c:608`, `nos/ui.c:772`), `sys_rec_*` (`nos/apps/sys_home.c`).

## Writes

Preferences. Each tile sets one field, then `prefs_save()` marks the prefs dirty (`nos/prefs.c:260-263`). The write is not yet durable.

| Tile | Field | Write site |
|---|---|---|
| Wi-Fi | `nos_prefs.wifi` | `sys_control.c:228` |
| Bluetooth | `nos_prefs.ble` | `:229` |
| DND | `nos_prefs.dnd` | `:230` |
| AOD | `nos_prefs.aod` | `:231` |
| Sound | `nos_prefs.sound` | `:232` |
| Haptics | `nos_prefs.haptics` | `:233` |
| LTE | `nos_prefs.lte` | `:234` |
| GPS | `nos_prefs.gps_auto` | `:235` |
| Raise | `nos_prefs.raise_wake` | `:236` |
| Brightness | `nos_prefs.brightness` | `:252` |

`prefs_save()` is also called on drag end and tween end (`:318`, `:338`, `:697`, `:744`).

Hardware.

- `hal_lte_enable(on)` (`:234`) and `hal_gps_enable(on)` (`:235`) set a stream flag only. The stream auto-stops after `NOS_EXT_IDLE_MS` = 8000 ms with no getter call (`firmware/main/ext/ext_gps.c:267-270`, `firmware/main/ext/ext_lte.c:302`, `nos/hal_ext.h:25-26`).
- `hal_set_backlight(pct * 255 / 100)` (`:253`) goes to `display_set_brightness` (`firmware/main/hal_esp32.c:437-440`).
- Wi-Fi and BLE have no direct call. The main loop applies the pref in `radio_preferences()` (`firmware/main/nos_main.c:953-961`), run at boot (`:1930`) and every loop pass while `mutate` (`:1987`). It calls `hal_wifi_set_enabled` and `hal_ble_set_enabled` (`firmware/main/hal_esp32.c:1934`, `:527`).

Diagnostics. Each toggle and each brightness drag opens a CONTROL action with `diag_action_request`, `diag_action_context` and `diag_action_result` (`sys_control.c:223-224`, `:242`, `:257`, `:263-264`; `nos/diag_log.h:34-57`). The result says SUCCESS when the local pref was written, not when the radio acknowledged (`:240-242`).

Screen stack. `nos_open`, `nos_open_icon` and `nos_push` (`:357`, `:615`, `:657`, `:705`, `:719`).

Not written: no service mutator, no alarm, notification, timer, wake config, phone link or Wi-Fi credential.

## Background behaviour

Control Center itself does nothing when closed. `tick_screens()` calls `c_tick` only for the top screen, or both screens during a transition (`nos/nos.c:2101-2110`). `c_tick` sets up the press state, reconciles tile fills, refreshes battery and time, drives the REC chip crossfade and calls `sys_control_prewarm()` (`sys_control.c:324-362`). `c_probe` runs each tick but `nos_motion_probe` is a no-op on device (`nos/nos.h:332`).

What keeps running for its settings, and who runs it:

- **Radios**: `radio_preferences()` in the main loop (`firmware/main/nos_main.c:953-961`, `:1987`). It is skipped while SD prefs are unresolved (`:957-958`), with no message. Once applied, the Wi-Fi and BLE drivers run on their own timers (`hal_esp32.c:1934-1956`, `:527-532`).
- **Prefs flush**: `prefs_tick()` runs from the awake loop (`nos_main.c:2162-2164`, gated on no touch and no transition), the screen-off loop (`:2098`), the AOD loop (`:2110`) and the engine frame (`nos/nos.c:2349`). It writes after 2 s of quiet or at 10 s maximum dirty age, and retries every 5 s on failure (`nos/prefs.c:366-384`).
- **LTE and GPS**: the `ext` task (`firmware/main/ext/ext_sys.c:144-155`, priority 0, 4 KB, PSRAM) runs `gps_service` and `lte_service` only while a stream is enabled or within its 8 s idle window.
- **Raise**: `wake_service_tick()` runs in the screen-off loop with eligibility from `raise_wake` (`nos_main.c:2038`, `core/wake_service.c:89-92`). The IMU sampling is in the same loop.
- **AOD and sensor idle**: read only at the idle timeout, `power_idle_target()` (`nos_main.c:2085`, `nos/power_policy.h:13-14`).
- **DND**: read when a notification is posted (`nos/svc.c:230`, `nos/nos.c:519`).
- **Sound and Haptics**: read in `nos_feedback` (`nos/nos.c:570-572`) and in the call-ring sound (`nos/svc.c:701`). Alarm and timer rings do not read them (Gaps).
- **Services with the screen off**: `nos_bg_tick(1.0f)` once per second (`nos_main.c:2094`) runs `svc_tick`, which runs `sys_tick` for battery (`nos/svc.c:1303-1322`).
- **Home prewarm**: while Home is idle, `home_blur_tick()` (`nos/apps/sys_home.c:640`, `:930-936`) calls `sys_control_prewarm()` to bake this sheet's backdrop into blur slot 6. The bake signature includes brightness and tile states (`sys_control.c:545-547`).

Screen off: the sheet is not running. Prefs still flush (`nos_main.c:2098`), radios still follow the pref, and the AOD or sensor decision is made at timeout.

## Persistence

Preferences. `nos_prefs` lives in RAM. `prefs_flush()` (`nos/prefs.c:331-365`) writes it through `hal_store_save_checked("nosprefs")` (`prefs.c:349`).

- SD: `/sdcard/watch/settings.0.bin` and `/sdcard/watch/settings.1.bin`, alternating slots with a checksum (`firmware/main/hal_esp32.c:243`, `nos/prefs_disk.c:22-32`). Both are protected from raw transfer (`firmware/main/sd_xfer.c:294`).
- NVS mirror: namespace `wos`, key `nosprefs` (`firmware/main/hal_esp32.c:236`, `:250`). An explicit edit overlay is kept under `prefs_overlay` (`hal_esp32.c:203-208`, `nos/prefs.c:49-58`).
- Failure: a failed SD write returns 0 and leaves the prefs dirty (`nos/prefs.c:349-353`). Retry every 5 s (`nos/prefs.h:57-58`). The sheet does not surface the failure (Gaps).
- Brightness is persisted when a drag or tween ends, not during the drag (`sys_control.c:318`, `:338`, `:697`, `:744`).

Diagnostics. CONTROL action records go to a RAM queue (`nos/diag_log.c:229-242`), drained by `diag_log_service()` into `/sdcard/watch/diagnostics/diag-NN.bin` (`nos/diag_log.c:24`, `:394-395`, `:463`; loop call `firmware/main/nos_main.c:1982`). This log is for development only and is not user-visible.

Not persisted: page, press state, note, animation and REC label live only in RAM (`sys_control.c:131-159`).

## Navigation

Opened from:

- Home face: drag down from page 0 with `|H.x| < 40` (`nos/apps/sys_home.c:1088-1090`, `nos_pull(&scr_control, TX_DROP)`). The upward drag opens Notifications instead (`:1091`).
- Serial command `open control` (`firmware/main/nos_main.c:1558`, `screen_by_name` `:924-935`).
- Simulator (`sim/nos_sim.c:70`).

Opens:

- Torch: `scr_torch` (`nos/apps/con_torch.c:217`), by tap or long press (`sys_control.c:615`, `:705`).
- Settings: `scr_settings` (`nos/apps/set_settings.c:1880`).
- Header tap: `scr_battery`, title BATTERY (`nos/apps/set_battery.c:94`, `sys_control.c:719`).
- REC chip: `sys_rec_screen(kind)` gives `scr_memo`, `scr_imu`, `scr_workout` or `scr_nav` (`nos/apps/sys_home.c:174`).
- Quick tools: `scr_quick_report`, `scr_energy`, `scr_battery_history` (`nos/apps/fit_energy.c:283-286`) and `scr_wrist_tuning` (`:287`), via `nos_push` (`sys_control.c:657`).

Closing: an upward drag belongs to the engine, which closes the sheet (`sys_control.c:9-10`). `c_input` does not take vertical drags (`:681`).

Returning from Torch or Settings: `c_enter` skips the reset when `nos_revealed()` is set, so the sheet comes back as it was (`:288`).

## Gaps

Ordered by user impact. Evidence is path:line.

1. **AOD tile does nothing under default prefs** (high). The idle target is `power_idle_target(low, sensor_idle, aod)`, which returns SENSOR before AOD whenever `sensor_idle` is set (`nos/power_policy.h:13-14`, `firmware/main/nos_main.c:2085`). `sensor_idle` defaults to 1 (`nos/prefs.c:159`) and is only toggled from ENERGY (`nos/apps/fit_energy.c:155`, `:247`). SENSOR and AOD both draw the AOD face (`firmware/main/nos_main.c:705`, `:2106-2108`). Impact: the AOD tile looks live but does not change the idle transition.
2. **Haptics and Sound do not silence alarm, timer, call or find-phone rings** (high). `ringer_tick()` calls `hal_haptic(HAPTIC_ALARM)` with no prefs check (`nos/svc.c:699`). Only the call-ring sound checks `sound` (`:701`). Gadgetbridge `vibrate` buzzes with no check (`:1188`). No file under `firmware/main/ext/` reads `nos_prefs`. Impact: Haptics off still buzzes at an alarm, and Sound off still sounds alarm and timer.
3. **DND only suppresses notification banners** (medium). Banner gate `nos/svc.c:230`, banner buzz `nos/nos.c:519`. The ring path (`nos/svc.c:688-705`) is not gated, so calls, alarms and timers still ring. No schedule, no per-app or call exception. Impact: DND is much narrower than the tile suggests.
4. **Sound and Haptics report present on a bare B with nothing verified** (medium). The speaker is set present with the comment "cannot be probed; default on" (`firmware/main/ext/ext_audio.c:343`). Haptics is present once the LEDC driver initialises (`firmware/main/ext/ext_haptic.c:109`), not when a motor responds. B has no built-in audio (`firmware/main/pins.h:64`). The strap pin map (`firmware/main/ext/ext_priv.h:33-44`) is the strap layout, which `CLAUDE.md` says still needs B-specific verification. Impact: enabled tiles for hardware that may not be there.
5. **LTE and GPS tiles show a saved preference, not module state** (medium). Streams auto-stop 8 s after the last getter (`firmware/main/ext/ext_gps.c:267-270`, `ext_lte.c:302`). Nothing applies `nos_prefs.lte` at boot. Its consumers are `nos/apps/con_cell.c:96`, `:194`, `:399` and this tile. `gps_auto` is also the sports GPS gate (`nos/sports.c:399`), so one tile carries two meanings. On a bare B the probes fail (`ext_gps.c:251`, `ext_lte.c:359`) and the tile shows NO GPS or NO LTE. Impact: a lit GPS tile does not keep GPS on.
6. **Wi-Fi and Bluetooth tiles show intent, not radio state; no phone status anywhere** (medium). `state()` reads the pref (`sys_control.c:173-174`). The radio follows in `radio_preferences()` (`firmware/main/nos_main.c:953-961`), skipped silently while prefs are unresolved (`:957-958`). `svc_sys().wifi_up` and `phone_connected` exist (`nos/svc.c:995-996`) but are not read. Turning Bluetooth off cuts the phone link with no warning (`firmware/main/hal_esp32.c:527-532`). Impact: the user cannot see whether Wi-Fi joined or the phone is connected.
7. **Battery and charging on B are not what the header implies** (medium). The header prints `battery_percent` and ignores `battery_valid` (`sys_control.c:345-346`). The field starts at 50 (`firmware/main/hal_esp32.c:287`) and is replaced only by the USB or stable-cell branch (`:350-361`, sampled once a second, `firmware/main/board.c:1021`). After 3 s stale, `hal_power_get` clears validity flags but not the percent (`hal_esp32.c:385-389`). "Charging" is `external_power == 1`, which on B means a USB host is connected (`hal_esp32.c:323`) or the rail is at or above 4.4 V for three samples (`:347-350`). `PIN_CHG_STAT` exists only on C (`firmware/main/pins.h:33`), not B. Impact: a false 50 % before the first valid sample, and the bright battery text means "USB host or high rail", not charger status. There is no charging icon, only colour.
8. **Brightness slider is overwritten by the frame loop** (medium; source-level, not bench-verified). The tile writes `pct * 255 / 100` (`sys_control.c:253`) through `display_set_brightness` (`firmware/main/nos_main.c:387-392`). Each awake frame calls `set_level(user_level())` (`nos_main.c:2225-2226`). `user_level()` is `6 + 2.49 * pct`, times `s_auto_k` when `auto_bright` is on (`nos_main.c:648-659`; default on, `nos/prefs.c:143`). `set_level` re-issues whenever the value differs from the last applied (`nos_main.c:661-666`). Impact: the level that sticks is not the one the bar wrote. With auto-brightness and a light sensor, the printed percent is not the panel level. The sheet has no way to turn auto-brightness off.
9. **"LOGS / WRIST TUNING" opens only WRIST TUNING** (low). `scr_wrist_tuning` is titled WRIST TUNING (`nos/apps/fit_energy.c:287`). Logs are behind ENERGY then LOGS (`fit_energy.c:205`, screen `scr_sports_logs` "SENSOR LOGS" at `:285`). Impact: the label promises a destination that is not opened.
10. **Failed prefs saves are silent** (medium). Toggles call `prefs_save()` only (`sys_control.c:239`, `:318`, `:697`) and never check a checkpoint. Settings reports `prefs_flush()` status (`nos/apps/set_settings.c:1765`, `:1819`), and main shows "PREFERENCES SAVE FAILED" (`firmware/main/nos_main.c:778`). A failed SD write keeps the pref dirty (`nos/prefs.c:349-353`). Impact: a toggle looks done when it is not yet durable.
11. **Raise is only a master gate** (low). The tile sets eligibility (`nos_main.c:2038`). The pickup and turn flags are in the persisted `wake_cfg` blob (`core/wake_service.c:44-49`), changed only by Wrist tuning or serial (`nos/apps/set_settings.c:1840`, `firmware/main/main.c:856-858`). Once a blob exists, the legacy default is ignored (`core/wake_service.c:57`). Impact: Raise can be on and still wake nothing. Not visible here.
12. **Torch is a screen, not a light toggle** (low). Tap or long press open `scr_torch` (`sys_control.c:613-616`, `:700-707`). Impact: one extra step for the most common quick action.
13. **Missing common quick controls** (low). No airplane mode (one action for Wi-Fi, Bluetooth, LTE and GPS). No lock tile, though `nos_set_locked()` exists (`nos/nos.c:636-645`) and `knock_lock` is in prefs (`nos/nos.c:629`). No volume or media control (`prefs.volume` is only in Settings). No theme toggle. Sensor-idle is only in ENERGY (`nos/apps/fit_energy.c:247`). The tile list is fixed at `sys_control.c:85-97`.
14. **12-hour mode has no AM/PM in the header** (low). `ui_fmt_time` drops the period (`nos/ui.c:901-905`). Alarms shows its own AM/PM tag.
15. **Redundant battery fallback** (low, code clarity). `battery_pct > 0 ? battery_pct : hal_battery_percent()` (`sys_control.c:345`, `:533`) reads the same snapshot (`nos/svc.c:991`, `firmware/main/hal_esp32.c:392`), so the second branch never adds a value.
