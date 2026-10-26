---
title: "Settings and developer"
tags: [nos, os-map]
updated: 2026-10-23
---

# Settings and developer

## Summary

Every user setting on the 1.43-B lives in one RAM struct, `nos_prefs_t` (52 bytes, `nos/prefs.h`). Screens write fields directly and call `prefs_save()`. The firmware main loop and the services read the fields live. Nothing subscribes to changes. `prefs_save()` only marks the struct dirty. A checked, fsync'd, read-back-verified write to two alternating SD banks (`/sdcard/watch/settings.0.bin` and `settings.1.bin`) happens later, from `prefs_tick()` on the single UI task (`app_main`, core 0, priority 1). That tick fires 2 s after the last edit, or 10 s after the first unsaved edit. An NVS mirror (namespace `wos`, key `nosprefs`) serves card-absent boots.

User-facing surfaces:

- **Settings** (`nos/apps/set_settings.c`): sectioned list plus picker, slider and set-time sub-pages.
- **Modules** (`set_modules.c`): 16-part status dashboard.
- **Diagnostics** (`set_diag.c`): guided test run.
- **About**, **Battery**, **Connectivity**.
- **Developer tools** (`set_developer.c`): overlays, frame-rate cap, TE sync, system trace.
- Five `fit_energy.c` screens: Energy, Battery history, Sensor logs, Quick report, Wrist tuning.

Wrist-gesture thresholds are not in `nos_prefs`. They live in `nos/tuning.c` and are saved as NVS key `sportstune`.

Main findings (details under Gaps):

- **No body profile.** The only body datum in the OS is `sports_mass_kg`, a `uint8_t` reachable only through Sports setup, default 0 (unset). There is no height, age, sex, weight or stride setting. Daily calories are `steps * 0.04` and distance uses a 0.75 m stride (`nos/svc.c:45-46`).
- **Dead prefs.** `face_accent` and `move_goal_10` are never wired to a feature. `torch` is a runtime flag stored in the persisted struct. `display_te` is unreachable on B because no TE pin is routed.
- **Saved haptic strength and speaker volume are not applied at boot.** Only the slider, Reset and Diagnostics push them to the drivers. The drivers start at 80 and 70 (`firmware/main/ext/ext_haptic.c:43`, `ext_audio.c:337`). The prefs defaults are 70 and 60.
- **The AOD toggle is overridden by the hidden `sensor_idle` pref.** It defaults to on, and its only UI is a button on Battery history.
- **Diagnostics results are not stored.** They go to a UART log and an 80-byte RAM ring that nothing in NOS reads.
- **Overridden or half-applied settings.** The phone silently overwrites the time-zone setting on every sync. Reset settings leaves the sports, wake and tuning blobs alone. The LTE intent is not applied at boot.
- **Legacy `core/settings.c` is still compiled** and `settings_load()` runs at boot; this contradicts the "unused" statement in [[Storage and persistence]].

## Components

### Settings app (`app-settings`)

- **Purpose:** the sectioned preference list. Rows are NAV, TOG, FLIP, ARM or DANGER types (`nos/apps/set_settings.c:426-486`).
- **Runs:** UI task. `settings_tick` runs every frame while the screen is on top and rebuilds value strings from cached HAL getters. It calls `set_modules_online()` about once a second and polls storage about every 2 s. Draw is pure: `tick` prepares all strings and animation values.
- **Reads:** all `nos_prefs` fields in the inventory, `hal_ext_present()` (strap mask), `hal_status`, `hal_lte_get`, `hal_get_datetime`, `svc_sys()`, `wake_service_get`, the face list.
- **Writes:**
  - `nos_prefs.*` followed by `prefs_save()` (toggle mapping `tog_pref` at `:524-536`).
  - `prefs_set_wrist()` for the Wrist flip; `wake_service_set` and `wake_service_learn` for reading angle.
  - `hal_set_backlight`, `hal_audio_volume`, `hal_haptic_set_strength` from sliders and Reset (`slide_apply`).
  - `hal_set_datetime_checked` from Set time (`:2361`).
  - `nos_host_power(1)` for Restart.
  - `diag_action_request/result` (domain CONTROL) for each change.
- **Sub-pages:**
  - Picker: screen timeout 5/10/15/30/60 s; time zone from a fixed offset table; step goal in 500-step increments (`pick_commit`).
  - Slider: brightness, AOD brightness, volume, haptic strength. The Auto toggle appears only with a light strap.
  - Set time: hour and minute drums, seconds forced to 0.
- **Arm-then-tap:** Restart and Reset settings arm on the first tap and act on the second (`ARM_S`). Restart saves and flushes prefs, then calls `nos_host_power(1)`. Reset calls `prefs_defaults()` then `prefs_flush()`. Actions: `flip` `:1741`, `reset_all` `:1755`, `activate` `:1774`.
- **Files:** `nos/apps/set_settings.c`.

### Modules (`sys-modules`)

- **Purpose:** bring-up dashboard of 16 parts (board and strap). Rows show status dot, bus string, live value and a detail page with a test action.
- **Runs:** UI task. Values refresh at 1 Hz. On entry it calls `hal_gps_enable(1)` and disables GPS on leave unless a track is active.
- **Shared with Settings:** `set_modules_online()`. The total is always 16, so a bare watch shows "n/16".
- **Detail actions:** RTC resync, SD retry, LTE enable (does not touch `nos_prefs.lte`), compass calibrate (saved by `ext_mag` into NVS `ext_mag`), HR and light enable, haptic and speaker patterns, live mic dB.
- **Files:** `nos/apps/set_modules.c` (`MI[]` part table, `presence`, `set_modules_online`).

### Diagnostics (`app-diagnostics`)

- **Purpose:** guided pass of four human cards (Button, Touch, Haptics, Speaker) while sensor tests run in the background, then a result list.
- **Tests** (`DI[]` thresholds): IMU |g| 0.80-1.20, RTC advancing, battery voltage window, SD mounted, mic floor, GPS NMEA, LTE AT answer, climate plausibility, light, compass |B| 20-70 uT, HR IR and contact. No tests exist for display, Wi-Fi, BLE, flash, PSRAM or charger.
- **Clocks:** advance only while the app is on screen. GPS, HR and LTE streams are started per test and powered down on resolve or abort.
- **Output:** `log_res` formats `DIAG <NAME> PASS|FAIL|ABSENT ...` and calls `hal_log()`. On device that is `ESP_LOGI` plus a RAM ring (`firmware/main/hal_esp32.c:471`). Nothing is persisted and `diag_log` is not used.
- **Side effects:** the haptic and speaker cards temporarily raise strength and volume, then `card_restore()` sets them back from prefs. This is the only place besides the sliders where saved values reach the drivers.
- **Files:** `nos/apps/set_diag.c`.

### Developer tools (`app-developer-tools`)

- **Purpose:** four session-only overlays, a frame-rate cap picker, a TE sync row and the System trace page.
- **Overlays** (`nos_dev_overlays`, bitmask `NOS_DEV_FPS=1`, `STATS=2`, `TOUCH=4`, `SCREEN=8`, `nos/developer.h`):
  - Loop FPS, averaged over 500 ms windows.
  - Frame stats: last raster time and dirty pixels.
  - Touch position.
  - Screen name and stack depth.
- **Persistence of overlays:** none. They reset at boot. System trace clears them on entry and restores them on exit (`nos/apps/set_developer.c:26-28`).
- **Frame rate:** `nos_prefs.fps_cap` in {24, 30, 40, 48, 60, 0 = unlimited}, persisted. The main loop reads it each iteration and re-seeds the pacer when it changes (`firmware/main/nos_main.c:2132`).
- **TE sync:** `nos_prefs.display_te`. On B the TE pin is not routed (GPIO8 is the IMU interrupt), so `nos_dev_te_available()` is 0 and the row reads "N/A" (`set_developer.c:119-121`).
- **System trace:** `diag_log_get` and `diag_log_fault_get` summary and fault pages, plus "ADD TEST MARK" which posts `DIAG_ACT_MARK`.
- **Hooks:** `nos.c:659` touch, `nos.c:1781` draw, `nos.c:2302` tick.
- **Files:** `nos/apps/set_developer.c`, `nos/developer.c`, `nos/developer.h`.

### About (`sys-about`)

- **Purpose:** scrolling page with logo, version, board name, display, memory, uptime, battery rail and font credits.
- **Hard-coded text:** board name and the display line "466 x 466 AMOLED, 30 fps" (`nos/apps/set_about.c:246-251`). The default frame cap is 60.
- **Live values:** refresh at 1 Hz.
- **Files:** `nos/apps/set_about.c`.

### Battery (`sys-battery`)

- **Purpose:** level hero numerals, ten-cell bar, power source, rail voltage, age of last good sample.
- **Reads:** `svc_sys()`, `hal_power_get`, `hal_battery_percent`, every tick.
- **Input:** none. The screen has no input handler, so it cannot link to Battery history.
- **Files:** `nos/apps/set_battery.c`.

### Connectivity pages (`sys-connectivity`)

- **Purpose:** Wi-Fi (scan, join, hidden network, phone setup) and Bluetooth (status, BLE scan, accessory).
- **Writes:** `nos_prefs.wifi` and `prefs_save()` on the Wi-Fi header row. Scans re-run every 3 s while open. Detail belongs to [[Connectivity and voice]].
- **Files:** `nos/apps/set_connectivity.c`.

### Energy, Battery history, Sensor logs, Quick report, Wrist tuning (`app-energy`, `app-battery-history`, `app-sensor-logs`, `app-quick-report`, `app-wrist-tuning`)

- **Structure:** one file, `nos/apps/fit_energy.c`. Five `nos_screen`s share draw, tick and input callbacks; each has its own state allocated with `nos_mem_alloc`.
- **Energy and Battery history:** 24-bar day chart from the sports journal via `sports_query_request/poll`, refreshed every 10 s. Energy adds food and water intake (`sports_intake`). Battery history has an "IDLE 80MHz ON/OFF" button that toggles `nos_prefs.sensor_idle` (`:155,247`).
- **Sensor logs:** browses sports-journal records per day, with SD status.
- **Quick report:** `report_enter` posts an unlabelled mark immediately (`:59`); SAVE LABEL posts a second, labelled one.
- **Wrist tuning:** adjusts `tuning_adjust` parameters for WALKING and KNOCK LOCK, starts labelled trials, calls `wake_service_learn` for other features. Flag `SCR_KEEPAWAKE`.
- **Files:** `nos/apps/fit_energy.c`, `nos/apps/registry.c:12-16`.

### Preferences owner (`svc-prefs`)

- **Purpose:** RAM `nos_prefs`, dirty tracking, field-level edit mask, merge on load, restore gate (IDLE, FLUSHING, HELD, RELOAD) for raw SD restores.
- **Debounce:** `prefs_tick` saves 2 s after the last edit or 10 s after the first. Failed saves retry every 5 s and stay dirty (`nos/prefs.c:366-384`).
- **Edit mask and overlay:** the edit mask tracks exactly which fields the user changed. Fields edited before the SD card resolves are merged field by field. The mask is saved as a checksummed overlay in NVS key `prefs_overlay` so card-absent edits survive a reboot.
- **Versioning:** `PREFS_VERSION 4`, internal `rev` 5. `normalize()` (`prefs.c:165`) upgrades older blobs.
- **Called from:** `nos_main.c:1923` (`prefs_load`), `nos.c:2349` (`prefs_tick` in `nos_frame_run`), and the loop windows below.
- **Files:** `nos/prefs.c`, `nos/prefs.h`.

### SD settings bank (`svc-settings-store`)

- **Format** (`nos/prefs_disk.c`): magic `0x4e4f5350`, format 1, sequence, size, 256-byte data area, FNV-1a checksum. About 276 bytes per slot.
- **Write:** to the slot that is not newest, fflush, fsync, then read back and compare. Skips the write if content is unchanged and never truncates an unreadable bank.
- **Glue:** `hal_store_save_checked` (`firmware/main/hal_esp32.c:228-256`) holds the SD asset lease and lock and mirrors to NVS. Only keys `nosprefs` and `wifi_cred` go to SD. Every other key is NVS only.

### Tuning (`svc-tuning`)

- **Struct:** `sports_tuning {version, walk_g, knock_g, walk_peaks, quiet_ms}`, defaults 0.10 g, 0.60 g, 8 peaks, 250 ms. Limits: walk_g 0.04-0.20, knock_g 0.20-1.2, peaks 4-12, quiet 80-400 ms.
- **Load:** at `svc_init` from NVS key `sportstune`.
- **Locking:** a spinlock (`portMUX`) is taken on every read.
- **Readers:** `firmware/main/board.c:270` (knock detector) and `:990` (pedometer), every IMU sample. Changes are live on the next sample.
- **Write:** `tuning_adjust` writes NVS synchronously on the UI task, bumps a revision and emits `SP_TUNE`.
- **Files:** `nos/tuning.c`, `nos/tuning.h`.

### Legacy cfg blob (`svc-legacy-cfg`)

- `core/settings.c` is compiled. `settings_load()` runs at boot (`nos_main.c:1921`), reads NVS blob `cfg`, sets `g_brightness` (default 200), calls `hal_set_backlight` and `wake_service_init`.
- `settings_save()` is never called, so `cfg` is never rewritten. Legacy globals are stubbed in `firmware/main/nos_legacy.c`.

### Progress indicators (`eng-prog`)

- **Purpose:** element-based progress drawing (`prog_bar`, `prog_ring`, `prog_arc`) in six styles: squares, segments, slants, triangles, chevrons, dots. Each element is one primitive for the damage recorder.
- **Cache:** an optional ring-geometry cache (two slots of 2088 bytes in PSRAM, with admission control) is enabled by default (`prog_geometry_enabled`).
- **Used by:** Battery page and other ring screens. Not a user setting.
- **Files:** `nos/prog.c`, `nos/prog.h`.

### Cost profiler (`eng-cost-profile`)

- **Purpose:** wall-clock cost buckets per frame (clear, sprite8, sprite565, text, shape, other).
- **Build:** `NOS_RENDER_PROFILING=1` is set in `firmware/main/CMakeLists.txt:73`, so it is always compiled in.
- **Control:** off by default. Serial commands `cost_probe`, `cost_calibrate` and bench; output lines `NOSCOST*`. No on-watch UI.
- **Files:** `nos/cost_profile.c`; frame bracketing in `nos/nos.c`.

### Native tile helper and scroll snapshots (`eng-native-tile`, `eng-scroll-diag`)

- **native_tile.h:** bounded vertical-exposure tile blits for Settings and launcher row sprites. The firmware builds with `NOS_RENDER_SHARP=1`, so the carry blit returns early with a plain `sprite8_blit` and the carry pool and shutter are inert.
- **scroll_diag.h:** per-screen `*_scroll_snapshot` functions. Used by the `scroll_state` and `nav_state` serial queries and by the frame loop to build the blur pose (`nos_main.c:1064-1078,2181-2192`).

### Diagnostics log (`svc-diag-log`, referenced)

- **Purpose:** bounded action and event log used by every Settings action and by System trace.
- **Storage:** bounded queue, 8 rotating files of 1 MiB at `/sdcard/watch/diagnostics/diag-00..07.bin` (`DLG1` records), plus an RTC_NOINIT ring and fault ledger that survive warm resets. Producers do no file I/O.
- **Drain:** `diag_log_service` from the sports SD writer task.
- **Files:** `nos/diag_log.c`, `nos/diag_log.h`.

## How it runs

- **Single UI task.** Settings, Modules, Diagnostics, About, Battery, Developer and the energy screens are `nos_screen`s ticked and drawn by `nos_frame_run()` on `app_main` (core 0, priority 1), up to 60 Hz while the screen is on ([[Engine and scheduling]]). They own no tasks. The only workers they touch belong to other subsystems: the board task (cached `hal_*` getters), the sports SD writer (history queries) and the panel flush path.
- **Live application of settings.** The main loop polls every iteration (`nos_main.c:1987`): `radio_preferences()`, `calendar_preferences()`, `display_preferences()` and `wake_wrist_sync(0)`. It reads `brightness`, `auto_bright`, `aod_bright`, `sleep_s`, `sensor_idle`, `aod`, `raise_wake` and `fps_cap` directly. `theme_apply` runs in the ON state. A pref edit therefore takes effect on the next loop pass unless the inventory says otherwise.
- **Persistence window.** `prefs_tick` writes only when the persistence flag is set. The loop sets it while the display is OFF, AOD or SENSOR (`nos_main.c:2098,2110`), or after a rendered frame when no touch, transition or capture is active (`:2160-2166`). The write (fsync plus read-back) runs inline on the UI task, so a slow card can stretch that frame.
- **Screen off.** Settings and Developer screens do not run when the screen is off. Pending prefs still flush from the loop (about 50 Hz in OFF and AOD loops). AOD and SENSOR states tick services once per second.
- **Explicit flush points.** Restart and Reset in Settings, shutdown, the `prefs flush` serial command, OTA and SD transfer paths that wait for `prefs_pending()`.
- **Modules and Diagnostics sampling.** They poll cached HAL snapshots (the IMU sample is valid if under 250 ms old). Streams (GPS, HR, LTE, mic) are enabled for the duration of a page or test.
- **Simulator.** `nos_hal_sim.c` stores blobs in files; strap-gated behaviour is simulated.

## Settings inventory

Defaults from `prefs_defaults()` (`nos/prefs.c:134-163`). "Live" means the change takes effect without restart. Device storage is `nosprefs` (SD banks plus NVS mirror) unless another store is stated.

| Field | Default | Set where | Consumers | Live? | Status |
|---|---|---|---|---|---|
| `haptics` | 1 | Settings toggle; Control Centre tile | `nos_feedback` (`nos/nos.c:570`), Breathe, HR | yes | works only with haptic strap (`EXT_HAPTIC`) |
| `haptic_strength` | 70 | Settings slider | `hal_haptic_set_strength` (slider, Reset, Diagnostics) | slider only | not applied at boot; driver starts at 80 |
| `sound` | 1 | Settings toggle; Control Centre tile | `nos_feedback` (`nos.c:572`), call ring, timer UI | yes | strap-only (`EXT_SPEAKER`); alarm and timer rings ignore it |
| `volume` | 60 | Settings slider | `hal_audio_volume` | slider only | not applied at boot; driver starts at 70 |
| `brightness` | 70 | Settings slider; Control Centre slider | `user_level()` (`nos_main.c:647`) | yes | works |
| `auto_bright` | 1 | Brightness page toggle (needs `EXT_LIGHT`) | `auto_brightness_tick` (`nos_main.c:653,675`) | yes | needs light strap |
| `aod` | 0 | Settings toggle; Control Centre tile | `power_idle_target` (`nos/power_policy.h`) | next idle | overridden by `sensor_idle` |
| `aod_bright` | 10 | Settings slider | `aod_level()` (`nos_main.c:660`) | yes | works; also SENSOR idle |
| `raise_wake` | 1 | Settings toggle; Control Centre tile | wake eligibility (`nos_main.c:2038`), `wake_service_init` | yes | works |
| `wrist_left` (+ `wrist_source`, `wrist_revision`) | 1 | Settings "Wrist"; Sports setup | `wake_wrist_sync` writes NVS `wake_cfg`; Sports | yes (1 s retry) | stored twice (prefs and `wake_cfg`) |
| `hour24` | 1 | Settings toggle | `ui_fmt_time` (`nos/ui.c:903`), faces (`nos/faces/faces.c:60-100`), alarms | yes | 12-hour shows no AM/PM except in Alarms |
| `dnd` | 0 | Settings toggle; Control Centre tile | banners (`nos/svc.c:230`), banner feedback (`nos.c:519`) | yes | no schedule |
| `face` | 0 | Faces picker | home screen | yes | works |
| `face_accent` | 0 | none | none | n/a | dead field |
| `sleep_s` | 15 | Settings picker 5/10/15/30/60 s | `power_timeout_due` (`nos_main.c:2083`) | yes | works |
| `torch` | 0 | Torch app | Torch app | n/a | runtime flag in persisted struct |
| `wifi` | 0 | Settings Wi-Fi page; Control Centre | `radio_preferences` -> `hal_wifi_set_enabled`; weather, voice | yes | works; waits for prefs load |
| `ble` | 1 | Control Centre tile only | `radio_preferences` -> `hal_ble_set_enabled` | yes | Settings comment says BT "stays on" (stale) |
| `lte` | 0 | Control Centre tile; Cellular app | `con_cell.c` keepalive | partly | not applied at boot; only Cellular inbox or reader screens re-enable the modem (`con_cell.c:473,486,614,644`) |
| `gps_auto` | 1 | Control Centre tile | gates SP_GPS journal rows (`nos/sports.c:399`) | yes | Settings row hard-rejected "GPS AUTO UNAVAILABLE" (`set_settings.c:1783-1789`) |
| `tz_min` | 330 | Settings picker; phone sync | `calendar_preferences` -> `board_apply_timezone`; world clock; weather | yes | phone sync overwrites it (`nos/svc.c:1003-1006`); fixed offset, no DST |
| `step_goal_100` | 80 | Settings picker | `svc.c:442` -> `svc_activity()->goal` | yes | works |
| `move_goal_10` | 0 | none | none | n/a | dead field |
| `units_metric` | 1 | Settings flip | Activity, Sports, Weather, Baro, Nav, Modules | yes | distance and temperature only |
| `theme` | 0 | Settings flip | `theme_apply` (`nos_main.c`) panel invert | yes | AOD stays dark |
| `knock_lock` | 1 | Settings toggle | `nos_knock_enabled` (`nos.c:629`) | yes | works |
| `sensor_idle` | 1 | Battery history button only (`fit_energy.c:155,247`) | `power_idle_target` (`nos_main.c:2085`) | next idle | hidden setting that changes what screen timeout does |
| `fps_cap` | 60 | Developer tools > Frame rate | `nos_main.c:2132` | yes | works |
| `display_te` | 0 | Developer tools (TE sync) | `display_preferences` (`nos_main.c:971`) | where available | not available on B (no TE pin) |
| `sports_dominant`, `sports_racket`, `sports_batting`, `sports_bowling`, `sports_volleyball`, `sports_auto_resume` | 0,0,0,0,0,1 | Sports hand/wrist setup | `fit_workout.c` | per session | Sports-owned |
| `sports_mass_kg` | 0 (unset) | Sports > setup > body mass | `workout.c:276` | per session | only body datum |
| `wrist_refs[4]` | 0 | Sports guided wrist calibration | `fit_workout.c` | per session | Sports-owned |

Settings persisted outside `nosprefs` (NVS namespace `wos`):

| Key | Written by | Notes |
|---|---|---|
| `sportstune` | `tuning_adjust` (`nos/tuning.c`) | NVS only; not on the SD banks; not in the overlay; not cleared by Reset settings. |
| `wake_cfg` | `wake_service_set` (`core/wake_service.c`) | Wrist side, thresholds, learned reading angle. |
| `ext_mag`, `ext_sea` | `ext_mag.c`, `ext_env.c` | Compass calibration and sea-level pressure. |
| `bat_cal` | serial `batcal` only | Battery divider calibration; no UI. |
| `voice_chat` | `firmware/main/voice_chat.c:524-537` | API key and models; set with a USB command; no watch UI. Contents not recorded here. |
| `cfg` | never written by NOS | Legacy; read once at boot (`core/settings.c`). |

## Data in / data out

| Direction | Peer | Data | Mechanism |
|---|---|---|---|
| in | touch (via `nos_input`) | taps, drags on lists, drums, sliders | screen `input` callbacks |
| in | HAL getters | strap presence, radio status, LTE, power, storage, datetime | direct calls, cached snapshots |
| in | phone (Gadgetbridge) | time zone and time | `svc.c` writes `nos_prefs.tz_min`, then `prefs_save` |
| in | serial commands | `bri`, `face`, `te`, `fps_cap`, `prefs`, `prefs flush`, `cost_probe` | handlers in `nos_main.c` |
| in | raw SD restore | settings files | `prefs_restore_begin/end` gate |
| out | main loop | radio intent, timezone, TE, wrist side, brightness, panel invert, sleep policy | polled globals |
| out | drivers | backlight, volume, haptic strength, RTC time | direct calls on user action |
| out | SD | `settings.0/1.bin` | `prefs_tick` -> `hal_store_save_checked` -> `prefs_disk_save` |
| out | NVS | `nosprefs`, `prefs_overlay`, `sportstune`, `wake_cfg` | `hal_store_*` |
| out | diag log | action request/result per setting change | `diag_action_*` |
| out | sports journal | `SP_TUNE`, `SP_LABEL`, report marks | `sports_emit`, `sports_report` |
| out | serial | `DIAG ...` lines, `NOSPREFS`, `NOSCOST` | `hal_log`, `printf` |

## Persistence

- **RAM:** `nos_prefs`, the tuning struct, `nos_dev_overlays` (session only) and all page state.
- **SD:** `/sdcard/watch/settings.0.bin` and `settings.1.bin` (authoritative `nos_prefs_t`); `/sdcard/watch/diagnostics/diag-00..07.bin` (event log).
- **NVS:** `nosprefs` mirror, `prefs_overlay`, `sportstune`, `wake_cfg`, `ext_mag`, `ext_sea`, `bat_cal`, `voice_chat`, `wifi_cred`, legacy `cfg`.
- **Reset settings:** `prefs_defaults()` zeroes the whole struct (including `wrist_revision`, `wrist_source`, `wrist_refs`, `sports_mass_kg` and all Sports prefs), then re-applies only backlight, volume and haptic strength (`set_settings.c:1755-1775`). It does not touch `sportstune`, `wake_cfg`, alarms, world clock, Wi-Fi credentials, `ext_*` or `voice_chat`.
- **Flash safeguards:** `tools/flash_watch.py` checkpoints the SD settings and refuses NVS ranges ([[Storage and persistence]]).

## Interactions

- [[Engine and scheduling]]: screens are ticked from `nos_frame_run`; `prefs_tick` runs there and in the loop windows; `fps_cap` re-seeds the pacer.
- [[Rendering pipeline]]: Settings row sprites, native carry (inert under SHARP), cost profiler and Developer overlays hook the frame; `theme` drives panel invert.
- [[Services hub]]: `svc_init` loads tuning; `svc.c` reads `step_goal_100`, `tz_min`, `dnd`, `sound`; phone time sync writes prefs.
- [[Health and activity]]: step goal, units, `sensor_idle`, Energy and Battery history screens.
- [[Sports and workouts]]: Wrist tuning labels and knock/walk thresholds feed the pedometer and knock detectors; sports prefs and body mass live in `nos_prefs`.
- [[Calorie trace]]: no body profile exists in Settings; `sports_mass_kg` is the only input and daily kcal is a flat per-step constant.
- [[Power and battery]]: Battery page, `sensor_idle`, `aod`, `sleep_s`, `aod_bright`; low-battery thresholds are fixed constants.
- [[Storage and persistence]]: prefs checkpoint, overlay, `hal_store_*` key routing.
- [[Hardware and drivers]]: strap presence gates most rows; Modules lists expected pins and addresses.
- [[Connectivity and voice]]: Wi-Fi and Bluetooth pages, radio intent polling, voice-chat key set only over serial.
- [[Shell and faces]]: Control Centre writes the same prefs (Wi-Fi, BT, DND, AOD, sound, haptics, LTE, GPS, raise); faces picker writes `face`.
- [[Utility apps]]: Cellular app and Torch share `lte` and `torch`.
- [[Gaps]] and [[OS overview]].

## Gaps

1. **High: saved haptic strength and volume are not applied at boot.**
   - Evidence: callers of `hal_haptic_set_strength` and `hal_audio_volume` are only in `nos/apps/set_settings.c` (slider and Reset) and `nos/apps/set_diag.c` (card restore). Drivers start at 80 (`firmware/main/ext/ext_haptic.c:43`) and 70 (`ext_audio.c:337`).
   - Impact: after reboot, haptics and sound ignore the saved levels until a slider, Reset or Diagnostics is touched, while Settings displays the saved value.
2. **High: the AOD toggle is overridden by a hidden pref.**
   - Evidence: `power_idle_target(low, sensor_idle, aod)` returns SENSOR whenever `sensor_idle` is set (`nos/power_policy.h:14`). Default is 1 (`nos/prefs.c`), and the only control is a button on Battery history (`nos/apps/fit_energy.c:155,247`).
   - Impact: with Always-on display OFF the screen still idles to a dim 1 Hz face; a true "off" cannot be reached from Settings and the battery cost is hidden.
3. **High: no body profile in Settings.**
   - Evidence: `nos_prefs_t` has no height, weight, age, sex or stride. `sports_mass_kg` defaults to 0 and is reachable only via Sports setup (`nos/apps/fit_workout.c:669-723`). `nos/svc.c:45-46` hard-codes the 0.75 m stride and 0.04 kcal per step.
   - Impact: distance and daily kcal are one-size-fits-all, workout kcal shows nothing when mass is unset, and heart-rate zones or basal rate cannot be built. See [[Calorie trace]].
4. **Medium: Diagnostics results are not persisted or visible after the run.**
   - Evidence: `log_res` only calls `hal_log` (`nos/apps/set_diag.c:126-142`); the ring behind it (`hal_esp32.c:471`) has no reader in NOS; `diag_log` is not used.
   - Impact: a bench run is lost unless a serial terminal is attached; no last-run history.
5. **Medium: Diagnostics omits display, Wi-Fi, BLE, flash, PSRAM and charger.**
   - Evidence: the `DI[]` table (`set_diag.c:60-77`) has no such entries.
   - Impact: a high pass count says nothing about radios or panel.
6. **Medium: time zone is overwritten by the phone and is offset-only.**
   - Evidence: `apply_time` replaces `nos_prefs.tz_min` on each Gadgetbridge time frame (`nos/svc.c:1003-1006`). The picker table is plain offsets. Default 330 is a specific zone and is also what Reset restores (`prefs.c:134-163`).
   - Impact: a manual zone choice reverts silently on next sync; no DST; Reset gives an arbitrary zone. There is no automatic versus manual setting.
7. **Medium: no date setter.**
   - Evidence: Set time has hour and minute drums only and reports "DATE NOT SET: SYNC PHONE" on an invalid year (`set_settings.c:2361-2368`).
   - Impact: with a lost RTC and no phone, the date cannot be set on the watch.
8. **Medium: 12-hour format shows no AM/PM outside Alarms.**
   - Evidence: `ui_fmt_time` (`nos/ui.c:903`) and the face formatter (`nos/faces/faces.c:60-100`) print hours modulo 12 without a suffix; AM/PM text exists only in the alarm screen.
   - Impact: "07:30" is ambiguous on faces and in Set time.
9. **Medium: LTE intent is not restored at boot, and its controls disagree.**
   - Evidence: `radio_preferences` handles only `ble` and `wifi` (`nos_main.c:956-962`). `nos_prefs.lte` is applied by `sms_keepalive` only inside Cellular inbox and reader screens (`nos/apps/con_cell.c:473,486,614,644`). The Modules LTE button calls `hal_lte_enable` without updating the pref (`set_modules.c:1380`). The Settings GPS row is hard-rejected (`set_settings.c:1783-1789`) while the Control Centre GPS tile works.
   - Impact: the LTE tile can show ON after reboot while the modem is off; `gps_auto` only gates a journal stream.
10. **Medium: dead or unreachable preferences.**
    - Evidence: `face_accent` and `move_goal_10` have no reader or writer; `torch` is a runtime flag; `display_te` has no hardware on B (`firmware/main/pins.h`, `nos_display_te.c`).
    - Impact: schema bytes unused; the Developer "TE sync" row is always "N/A" on B; no active-minutes or kcal goal exists despite the reserved byte.
11. **Medium: opening Quick report writes a journal mark.**
    - Evidence: `report_enter` calls `sports_report(E.feature,-1,E.seconds)` (`fit_energy.c:59`).
    - Impact: every visit adds an "unlabelled mark" and SAVE LABEL adds a second record, polluting training data.
12. **Medium: Wrist tuning has misleading help and few knobs.**
    - Evidence: `tuning_adjust` handles only walk and knock parameters; other features show "LEARN VIEW: HOLD WATCH TO READ" (`fit_energy.c:203`).
    - Impact: raise-to-wake sensitivity and similar cannot be tuned on the watch; labels for several features are misleading.
13. **Medium: `sportstune` and `wake_cfg` are NVS-only.**
    - Evidence: `hal_store_save_checked` routes only `nosprefs` and `wifi_cred` to SD (`hal_esp32.c:228-256`).
    - Impact: they do not follow the SD-authoritative rule in CLAUDE.md, are outside the overlay, and are lost by an NVS erase; Reset settings leaves them.
14. **Low: Reset settings is partial and surprising.**
    - Evidence: `prefs_defaults()` zeroes the whole struct; Reset re-applies only three driver values (`set_settings.c:1755-1775`); `sportstune`, `wake_cfg`, credentials stay.
    - Impact: Sports body mass and hand prefs are lost together with display prefs, while gesture tuning survives.
15. **Low: About has hard-coded and thin content.**
    - Evidence: board name and "30 fps" literals (`set_about.c:246-251`) versus default cap 60 (`nos/frame_cap.h`). No flash size, MAC, SD size, reset reason or OTA slot.
    - Impact: the page misstates frame rate; support questions need serial.
16. **Low: Battery page has no navigation and no battery-saver setting.**
    - Evidence: `scr_battery` has no input handler (`set_battery.c`); low-battery thresholds are constants in `nos/power_policy.h`.
    - Impact: battery-saver behaviour is fixed and not visible in Settings.
17. **Low: Modules "n/16" can never reach 16 on a bare watch.**
    - Evidence: `set_modules_online` totals 16 parts, counting absent straps as not online (`set_modules.c:249`).
    - Impact: the Settings Modules row always looks degraded.
18. **Low: Settings tick runs HAL polls every second.**
    - Evidence: `set_modules_online` and storage polling from `settings_tick` (`set_settings.c:677-686`).
    - Impact: small per-frame CPU cost while Settings is open.
19. **Low: Settings render caches are partly dead in the sharp build.**
    - Evidence: `NOS_RENDER_SHARP=1` (`firmware/main/CMakeLists.txt:73`) disables the shutter and native carry (`nos/apps/native_tile.h`); row caches are still allocated.
    - Impact: wasted PSRAM and code while the Settings cache is resident.
20. **Low: synchronous SD/NVS writes run on the UI task.**
    - Evidence: `prefs_flush` in Restart and Reset, `tuning_adjust` NVS write (`nos/tuning.c`), and the debounced `prefs_tick` write with fsync and verify.
    - Impact: a hitch of tens to hundreds of ms on a slow card.
21. **Low: loaded prefs are only partly range-checked.**
    - Evidence: `normalize()` (`prefs.c:165`) clamps `sleep_s` and sports enums but not brightness, volume, haptic strength, `tz_min`, step goal or `aod_bright`.
    - Impact: a hand-restored file could push out-of-range values into drivers.
22. **Low: Developer overlays and cost probe have no persistence and no on-watch cost UI.**
    - Evidence: `nos_dev_overlays` is a plain global; `cost_probe` is serial-only (`nos_main.c`).
    - Impact: the compiled-in profiler needs a PC.
23. **Low: voice-chat key is NVS plaintext and USB-only.**
    - Evidence: `firmware/main/voice_chat.c:524-537`.
    - Impact: no on-watch key entry; key stored unencrypted. (Value not recorded here.)
24. **Low: boot backlight uses the legacy value.**
    - Evidence: `settings_load()` sets `g_brightness` 200 and calls `hal_set_backlight` (`core/settings.c`, `nos_main.c:1921`) before `prefs_load()` at `:1923`.
    - Impact: the boot animation can show at about 78 % before the user level applies.
25. **Low: DND is minimal.**
    - Evidence: `dnd` gates only banners and banner feedback (`nos/svc.c:230`, `nos/nos.c:519`).
    - Impact: no schedule, no per-app filter; rings and calls ignore it.
26. **Low: stale comment and doc drift.**
    - Evidence: `set_settings.c:17-19` says Bluetooth is status only, while Control Centre toggles `nos_prefs.ble`. [[Storage and persistence]] says `core/settings.c` is unused, but `settings_load()` runs at boot.
    - Impact: documentation drift only.

## Missing settings that features need

- Body profile: height (stride and distance), weight (workout and basal kcal), age or birth year, sex (BMR, HR max). Today only `sports_mass_kg` exists.
- Daily calorie and active-minute goals (`move_goal_10` is a reserved byte).
- Date setter, automatic versus manual time zone, DST handling.
- AM/PM indicator, date format, first day of week.
- Battery saver switch and thresholds; sensor idle in Settings with honest AOD wording.
- Notification schedule (DND) and per-app filter.
- On-watch voice-chat key entry.
- Raise-to-wake sensitivity, pickup and turn-wrist knobs.
