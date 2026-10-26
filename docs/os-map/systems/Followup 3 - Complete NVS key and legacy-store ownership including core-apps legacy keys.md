---
title: "Complete NVS key and legacy-store ownership including core/apps legacy keys"
tags: [nos, os-map]
updated: 2026-10-23
---

# Complete NVS key and legacy-store ownership including core/apps legacy keys

## Summary

- Every `hal_store_*` call in `nos/`, `core/`, `firmware/main/` was grepped. The 1.43-B NOS build **actively writes 14 NVS keys** (`nosprefs`, `wifi_cred`, `prefs_overlay`, `bat_cal`, `pedometer`, `nosact`, `nosalarm`, `nos_wc`, `sportsfood`, `sportsmarks`, `sportstune`, `wake_cfg`, `ext_sea`, `ext_mag`). It reads but never writes two more (`cfg`, `weather_loc`). Only `nosprefs` and `wifi_cred` go to SD first (`firmware/main/hal_esp32.c:236`); the other 12 written keys are NVS only.
- The previous inventory missed `wake_cfg` (core/wake_service.c, live), `cfg` (core/settings.c, load-only in NOS), `ext_sea`, `ext_mag` (strap drivers) and `weather_loc` (load-only). It also listed `alarms`, `pip_memory`, `pip_care_a`, `pip_care_b`, `lcd_gap` as if live. **Those five are legacy-only and not compiled into the B build** (see Components, legacy).
- Build membership is decided by `firmware/main/CMakeLists.txt:14-45`. `main.c`, `core/wos.c`, `core/apps/*` and `core/pip*.c` are NOT in the source list. `nos_main.c` supplies `app_main` (`firmware/main/nos_main.c:1899`); `main.c` also defines `app_main` (`firmware/main/main.c:1043`) and is excluded. `core/walk.c` IS built (`CMakeLists.txt:17`) but owns no store.
- `voice_chat` and `voice_hist` are written only on the 1.43C. On B, `voice_chat.c` compiles only no-op stubs (`firmware/main/voice_chat.c:596-615`), so neither key is read or written.
- NVS is the 24 KB `nvs` partition at 0x9000 (`firmware/partitions.csv:5`), 6 sectors. Using ESP-IDF's accounting that is 756 entries of 32 B, with one page reserved for garbage collection, so about 630 usable. `sportsfood` alone is 8200 B, roughly 262 entries. Because ESP-IDF writes the new blob version before erasing the old one, a rewrite needs about 524 entries for that key alone. The model in "Persistence" suggests the second and later food-ledger saves cannot fit once the PHY calibration blob and the other keys are present. **This is a calculation, not a device measurement.**
- Failure behaviour is weak: void `hal_store_save` callers (`nos_wc`, `ext_sea`, `ext_mag` calibration, `bat_cal`) lose data silently. `svc_alarm_save` does not retry. A failed `pedometer` save blocks shutdown. The Settings "SETTINGS STORE %" gauge is sampled once at boot and divides by a total that includes the reserve page.

## Components

### svc-settings-store - `hal_store_*` blob API (NVS namespace `wos`)
- Purpose: one blob store. `nvs_save` opens namespace `wos` READWRITE, `nvs_set_blob`, `nvs_commit` (`firmware/main/hal_esp32.c:177-188`). `hal_store_save_checked` routes only `nosprefs` and `wifi_cred` to SD checkpoints, then mirrors to NVS (`hal_esp32.c:228-255`). Every other key returns `nvs_save()` directly (`:236`). `hal_store_load` reads SD first for the same two keys, falls back to NVS only when the SD status is MISSING, and otherwise returns 0 (`:258-282`).
- Where it runs: the caller's task and stack (UI task, `board-fs`, `wifi-settings`, `cmd`). NVS flash writes must run on an internal-RAM stack; `board.c:1032-1034` comments say the PSRAM-stack `board-sense` task never does NVS or filesystem writes.
- Cadence: per caller (see key table). Identical-content blob rewrites are a no-op in ESP-IDF (`cmpMultiPageBlob`, `esp-idf/components/nvs_flash/src/nvs_storage.cpp:~480-500`), so the unconditional NVS mirror of `nosprefs`/`wifi_cred` costs no flash when the bytes are unchanged.
- Reads/writes: NVS `wos/<key>`; SD `/sdcard/watch/settings.{0,1}.bin` (`nosprefs`) and `/sdcard/watch/wifi/settings.{0,1}.bin` (`wifi_cred`) through `nos/prefs_disk.c:81-120` (276 B checkpoint: 16 B header + 256 B payload + 4 B CRC, alternating banks, fsync and readback).
- `!BOARD_HAS_SD` (1.43C) sends everything to NVS (`hal_esp32.c:231-233`); `BOARD_HAS_SD` is 1 for B (`firmware/main/pins.h:63`).
- Failure behaviour: `nvs_save` returns 0 on any `nvs_open`, `nvs_set_blob` or `nvs_commit` error, including `ESP_ERR_NVS_NOT_ENOUGH_SPACE`. For the SD-routed keys the NVS mirror result is discarded (`(void)nvs_save`, `:250`), so a full NVS does not fail a successful SD save.
- NVS init: `nvs_flash_init` in `hal_esp32_init_periph` (`hal_esp32.c:2316-2323`). On `NO_FREE_PAGES` or `NEW_VERSION_FOUND` it only logs "preserving saved configuration" and continues WITHOUT an initialised NVS. After that every `nvs_open` fails, so all loads return 0 and all NVS saves return 0, with no user-visible alarm.

### svc-prefs - `nosprefs` + `prefs_overlay` (nos/prefs.c)
- Purpose: 52-byte `nos_prefs_t` (`_Static_assert sizeof==52`, `nos/prefs.c:14`) with SD-authoritative checkpoint plus NVS fallback; `prefs_overlay` is an 80-byte (computed: `uint32 magic,version; uint64 mask; uint32 wrist_base; nos_prefs_t value; uint32 check`, `prefs.c:49-56`, cap 256) per-field edit overlay used only while the SD state is unknown or the save failed.
- Runs: UI task (`app_main`, core 0, prio 1). `prefs_save()` only marks dirty (`prefs.c:~258-264`). `prefs_tick()` (`prefs.c:366-384`) flushes when dirty and `(now-dirty_ms>2000 || now-first_dirty_ms>=10000)` and not within 5 s of a failed attempt. It is called (a) post-frame when no touch, injection, transition or capture is active (`nos_main.c:2160-2166`), and (b) in the OFF and AOD/SENSOR loops (`nos_main.c:2098`, `:2110`). `prefs_flush()` is called on shutdown (`nos_main.c:778`, `:796`), power restart (`:834`) and serial `prefs flush` (`:1624`).
- Writes: `hal_store_save_checked("nosprefs",...)` (`prefs.c:349`) then `overlay_checkpoint`. `overlay_checkpoint(0)` (a tombstone-free full overlay) runs only after a failed SD save (`prefs.c:350-352`). The first success writes one empty tombstone to `prefs_overlay` when edits were pending; after that the tombstone compares equal and is skipped (`prefs.c:121-128`).
- Reads: `prefs_load()` at boot (`nos_main.c:1923`), with a V3-size legacy migration (`prefs.c:196-209`) and `overlay_resolve` (`prefs.c:~98-117`).
- Persistence: SD banks authoritative. NVS `nosprefs` is a mirror written on every flush attempt, including when no card is present (`hal_esp32.c:249-251`). With no card `leased` is 0, `prefs_disk_save` is skipped, the call returns 0, the data stays dirty and retries every 5 s; the repeated NVS mirror write is a no-op after the first.

### svc-wifi - `wifi_cred` (hal_esp32.c wifi worker)
- Purpose: 98-byte `wifi_cred_t{char ssid[33]; char pass[65]}` (`hal_esp32.c:1987`). SD `/sdcard/watch/wifi/settings.{0,1}.bin` is authoritative, with an NVS mirror. The Wi-Fi driver itself is set to `WIFI_STORAGE_RAM` (`hal_esp32.c:2286`), so the driver keeps no second copy of the password.
- Runs: task `wifi-settings`, `xTaskCreate` prio 2, 6144 B internal stack, no core affinity (`hal_esp32.c:2268`). Save fires only after an association whose AP SSID matches the candidate (`:2041-2046`). It is retried every 5 s while `s_credentials_dirty` (`:2050-2053`). It takes `ota_external_activity_begin()` so it cannot overlap OTA (`:1998-2005`).
- Reads: `hal_store_load("wifi_cred")` at radio start (`hal_esp32.c:2297`).
- Screen-off: unaffected, the worker is independent of the display.

### svc-pedometer - `pedometer` (board.c)
- Purpose: 24-byte `pedometer_saved{u32 version,day,steps,reserved; u64 active_ms}` (`core/pedometer.h:4-8`). Load at `board_init` (`firmware/main/board.c:1069`). Saved by `save_pedometer` (`board.c:903-918`) only when the snapshot differs from the last saved copy and the calendar day is resolved (not `prefs_loading()`).
- Runs: task `board-fs` (`xTaskCreateWithCaps`, prio 1, 4096 B internal stack, `board.c:1042`). Cadence: every 60 s after a success, 5 s retry after a failure (`board.c:954-956`), but only while `s_background_persistence` is true. The UI sets that flag to 0 while the screen is ON and to 1 in OFF/AOD/SENSOR (`nos_main.c:2098`, `:2110`, `:2141`; `board.c:46-47`). Final save when a sleep or shutdown is requested (`board.c:931-940`).
- Failure: a failed final save sets `s_fs_sleep_ok=0`, so `board_prepare_sleep` returns failure (`board.c:1146-1152`), and the shutdown FSM cancels with `DIAG_SLEEP_BLOCK` 16 (`nos_main.c:799-801`). A full NVS can therefore block power-off or restart whenever steps changed since the last save.

### svc-activity - `nosact` (nos/svc.c)
- Purpose: 92-byte `act_saved` (`svc.c:427-434`, computed): hourly steps, six previous days, active minutes.
- Runs: UI task, `svc_tick` -> `act_tick` once per second (`svc.c:1303-1318`); with the screen off it still ticks once per second via `nos_bg_tick(1.0f)` (`nos_main.c:2093`). Save condition: dirty and more than 10 min since the last save, or 5 s after a failed save (`svc.c:558`), plus an immediate save at day rollover (`svc.c:527`). The save is synchronous in the frame or service call, with no power-state gating.
- Reads: `act_init` (`svc.c:562-577`).
- Failure: sets dirty and retry, increments `s_act.checkpoint_errors`, logs `DIAG_STORAGE_ERROR` (`svc.c:496-503`). Nothing reads `checkpoint_pending`, `checkpoint_errors` or `checkpoint_epoch` outside `svc.c` (grep), so the user never sees it.
- No shutdown flush: `act_save` is called only from `act_rollover` and `act_tick`; the shutdown path flushes prefs, voice history and sports, not `nosact`. Up to 10 minutes of hourly steps can be lost on power-off, restart or crash.

### svc-alarms - `nosalarm` (nos/svc.c)
- Purpose: 164-byte `alarm_saved{u8 version,count,pad[2]; svc_alarm a[8]}` (`svc.c:~712`, `svc.h:119-120`, computed: 4 + 8 x 20 B). Schema version 1.
- Runs: UI task. `svc_alarm_save()` (`svc.c:721-730`) is called on add (`:751`), edit (`time_alarm.c:97`), remove (`:766`), a one-shot alarm firing (`:841`) and a Gadgetbridge alarm sync (`:1137`). `alarm_load` at `svc_init` (`:732-742`, `:1288`).
- Failure: the checked result goes only to `action_trace` (DIAG_RESULT_FAILED); there is no dirty flag and no retry, so RAM and NVS stay divergent until the next edit. The UI is not told.

### app-world-clock - `nos_wc` (nos/apps/time_worldclock.c)
- 6-byte favourites struct `{u8 magic,n,idx[4]}` (`time_worldclock.c:44`). `fav_load` lazily on first picker entry (`:52`), `fav_save` on every toggle via void `hal_store_save` (`:60`, `:334`). UI task, in frame. A failed save is silent.

### svc-sports-logger - `sportsfood`, `sportsmarks`, `sportstune` (nos/sports.c, nos/tuning.c)
- `sportsfood`: 8200 B `{u32 version, serial; sports_record entries[128]}` (`sports.c:70`; `sports_record` is 64 B). `sports_intake` (`sports.c:~337-349`) and `sports_undo` (`:~351-363`) save with `hal_store_save_checked` and roll the RAM ledger back on failure. User-initiated from Energy (`nos/apps/fit_energy.c:238`, `:249`). Loaded in `sports_init` (`sports.c:310`).
- `sportsmarks`: 1032 B `{version, serial; records[16]}` (`sports.c:202`, load `:313`). Written by `sports_report` (`fit_energy.c:59`, `:261`), rolled back on failure.
- `sportstune`: 20 B `sports_tuning{u32 version; float walk_g, knock_g; int walk_peaks, quiet_ms}` (`nos/tuning.h:3`). `tuning_adjust` saves on each +/- tap (`nos/tuning.c:40`, UI `fit_energy.c:267`, `:277`); load `tuning_init` (`tuning.c:18`).
- Runs: UI task, in frame, synchronous. Retained entries are replayed to the SD journal by the `sports_sd` task (`sports.c:320-321`, prio 1), which does no NVS.
- A save failure shows "SAVE FAILED" / "MARK SAVE FAILED" in Energy; this is the only checked-and-surfaced failure path.

### svc-motion-wake - `wake_cfg` (core/wake_service.c)
- 48-byte `wake_blob{magic,version,flags,wrist,learned; int32 view[2][3]; checksum}` (`wake_service.c:8-12`). Checksummed (`:22-27`, `:29-40`) and written with `hal_store_save_checked("wake_cfg")` in `save()` (`:60-66`).
- Writers: `wake_service_set` (`:68-80`) from Settings (`nos/apps/set_settings.c:1840`) and from `wake_wrist_sync` (`nos_main.c:939-950`, retry 1 s on failure and called every loop pass); learn completion inside `wake_service_tick` (`wake_service.c:119`) driven from the UI loop (`nos_main.c:2038`).
- Load: `wake_service_init` (`:51-58`), called twice at boot, first by `settings_load` (`core/settings.c:64`) and then `nos_main.c:1951`.

### svc-settings-store (legacy blob) - `cfg` (core/settings.c, live code, dead data)
- 40-byte `cfg_blob{magic + 9 int32}` (`core/settings.c:29-34`). `settings_load()` runs at boot (`nos_main.c:1921`) and applies brightness, 24 h, timezone etc. to legacy globals defined in `firmware/main/nos_legacy.c`. **`settings_save()` is never called in the NOS build** (grep `nos/`, `firmware/main/` excluding `main.c`), so `cfg` is a frozen legacy blob. It tries a second shorter read when the full read fails (`settings.c:42-43`).

### Strap calibration stores - `ext_sea`, `ext_mag` (firmware/main/ext)
- `ext_sea`: 4-byte float. Loaded when the barometer chip probes (`ext_env.c:113`). Saved by `hal_env_set_sea_level` via void `hal_store_save` (`ext_env.c:192-197`), called from the Barometer calibrate screen on UI (`nos/apps/out_baro.c:686`). The UI treats the save as success without an ACK (comment at `out_baro.c:688`).
- `ext_mag`: 36-byte `mag_cal` (`ext_mag.c:37-43`, computed 34 padded to 36). Loaded in the mag probe (`:97`). Saved when calibration finishes (`hal_mag_calibrate(0)`, `ext_mag.c:260`, void save) and by the `ext mag map` debug command with the checked variant (`:294`). Calibration is triggered from Compass (`nos/apps/out_compass.c:186,212,355,408`) and Settings > Modules (`nos/apps/set_modules.c:959-1391`).
- Both exist only when the strap hardware responds.

### svc-weather-fetch - `weather_loc` (firmware/main/weather_service.c)
- 44-byte `weather_config{u32 version; float lat,lon; char city[32]}` (`weather_service.c:18`). Loaded in `weather_service_start` (`:163`). Saved by `hal_weather_set_location` (`:30-37`, checked). **In the NOS build nothing calls `hal_weather_set_location`**: its only product caller is the legacy serial command in `firmware/main/main.c:776` (not compiled). The key is therefore load-only; a B watch can only use a location left by an earlier legacy firmware. This was not verified against the phone-setup page `watch_setup.html`: no `hal_weather_set*` call exists in `hal_esp32.c` or `nos_main.c`, so it appears not to set it either.

### Battery calibration - `bat_cal` (hal_esp32.c)
- 4-byte int mV offset. Loaded lazily by the first `hal_esp32_sample_power` (`hal_esp32.c:318`), called from `board_init` before workers start (`board.c:1073`). Written by `hal_battery_cal_set` (`:314`, void) from serial `batcal MV` (`nos_main.c:1607`) on task `cmd` (`xTaskCreate`, prio 4, 6144 B).

### svc-voice - `voice_chat`, `voice_hist` (1.43C only)
- On B these are stubs (`voice_chat.c:596-615`, guarded by `#if CONFIG_WATCH_BOARD_143C` at `:7`). On C: `voice_config` 228 B (computed: 4 + 128 + 64 + 64 + 32), `voice_history` ~3892 B (`nos/voice_history.h`, computed: 12 + 4 + 4 x 968 + 4 + 4 header and checksum bytes). `voice_chat_init` opens NVS directly with `nvs_open("wos")`, bypassing `hal_store_load` (`voice_chat.c:98-103`). `voice_chat_maintenance` (UI loop, `nos_main.c:777`, `:795`, `:1986`) writes `voice_hist` with a read-back verify and 5 s retry (`voice_chat.c:199-211`). `voice_chat` is written by the `configure` command (`:534-535`). Not counted in the B capacity model.

### Legacy (not built) - svc-legacy-main, svc-legacy-core-apps, svc-legacy-pip
- `firmware/main/main.c` (legacy entry, 1345 lines): not in `CMakeLists.txt` SRCS. Holds `lcd_gap` load at `main.c:426` (inside `#if CONFIG_WATCH_BOARD_143C`, so even the legacy B build never read it) and save at `main.c:826` (serial `gap N`; 4-byte int).
- `core/apps/app_alarm.c:96-108`: key `alarms`, 72 B (`alarm_t{int hour,minute,enabled}` x 6 slots). NVS-only, loaded lazily on app open, saved on every edit. Not migrated into `nosalarm` (NOS `alarm_load` reads only `nosalarm`, `svc.c:732-742`).
- `core/apps/face_pip.c:40-44`, `:106-112` with `core/pip.c`, `core/pip_care.c`: `pip_memory` 20 B (`core/pip.h:13-15`), `pip_care_a` / `pip_care_b` 176 B each (computed from `pip_care_memory`, `core/pip_care.h:47-56`). `pip_care_snapshot` sets `generation = memory.generation+1` (`pip_care.c:389-394`); the save key is `snapshot.generation&1 ? "pip_care_a" : "pip_care_b"` (`face_pip.c:111`); load picks the valid bank with the larger signed generation (`:43-47`). Saves use `hal_store_save_checked`, rolled back to dirty and retried on failure. Cadence is `save_now`, or 300 s, or 15 s after an episode settles and at least 30 s since the last save (`face_pip.c:102-104`).
- All of the above are dead code in the NOS build: `CMakeLists.txt:14-20` lists only `asset_job gfx font sprite anim battery pedometer walk notif motion_wake wake_service settings knock ephem` from `core/`, and `nos/` has no reference to `pip_*`, `face_pip` or `core/apps` (grep). `core/apps` is still compiled by the archived simulator (`tools/build_sim.py:85`, `sim/build.ps1:1-5`), and `sim/main_win32.c` is the legacy sim host.
- `core/walk.c` is live (compiled, `CMakeLists.txt:17`; used by `core/pedometer.c:13-18`) and owns no store.
- Their NVS data persists on a watch that ever ran legacy firmware, because NVS is never erased (`hal_esp32.c:2318-2320`, `tools/flash_watch.py:33,150`). It costs entries but nothing reads it.

## How it runs

- **UI task** (`app_main`, core 0, prio 1; `sdkconfig` `ESP_MAIN_TASK_AFFINITY=0x0`): does the checked NVS writes for `nosprefs`, `nosact`, `nosalarm`, `nos_wc`, `sportsfood`, `sportsmarks`, `sportstune`, `wake_cfg`, `ext_sea`, `ext_mag`, `prefs_overlay` (and `voice_*` on C). All are synchronous `nvs_set_blob` + `nvs_commit` calls inside a frame or service tick. Only `nosprefs` is debounced (2 s quiet / 10 s max, gated to windows with no touch, transition or capture, `nos_main.c:2160-2166`). The others fire on the user action or on their own timers (`nosact` 10 min). A flash page erase during GC can occur inside such a write. `docs/OPTIMIZATIONS_DEVICE.md:28` itself says direct store calls remain synchronous and a wake can meet a flash operation in flight.
- **`board-fs`** (prio 1, internal stack, unpinned): `pedometer` NVS, minute-log SD writes, RTC maintenance; 100 ms loop.
- **`wifi-settings`** (prio 2, unpinned): `wifi_cred`.
- **`cmd`** (prio 4): `bat_cal` on serial command.
- **Screen off**: the UI loop keeps running (20 ms delay, services once per second, `nos_main.c:2091-2102`), so `nosact`, `nosalarm` and `wake_cfg` writes still happen. `nosprefs` and `pedometer` writes are deliberately deferred until OFF/AOD (`board_set_background_persistence(1)`). `wifi_cred` is independent of the screen.
- Boot read order (`nos_main.c:1915-1951`, `board.c:1066-1074`): NVS init -> `board_init` (SD mount, `pedometer`, `bat_cal`) -> `settings_load` (`cfg`) -> `prefs_load` -> `hal_ext_init` (`ext_sea`/`ext_mag` after probes) -> `voice_chat_init` -> radios (`wifi_cred`, `weather_loc` when weather starts) -> `nos_apps_boot` (`svc_init`: `nosalarm`, `nosact`, `sportstune`, `sports*`) -> `wake_service_init` (`wake_cfg`). A lazy read of `nos_wc` happens on first Time/World clock picker use.

## Data in / data out

Key table for the B build. "Route" = where `hal_store_save*` sends it. Sizes are `sizeof` computed from struct layout unless a `_Static_assert` exists (nosprefs) or the comment gives a figure (sportsfood = 8200, sportsmarks = 1032 match computed). Entries use ESP-IDF's blob accounting: 1 index entry + 1 header + ceil(bytes/32) data entries per chunk.

| Key | Size B | Entries | Owner (file) | Writer cadence | Context | Route | Live on B |
|---|---|---|---|---|---|---|---|
| `nosprefs` | 52 | ~4 | `nos/prefs.c:349` | debounced 2 s / 10 s max, 5 s retry | UI, post-frame/OFF/AOD | SD banks + NVS mirror | yes |
| `prefs_overlay` | 80 | ~5 | `prefs.c:121-128` via `hal_esp32.c:203` | on failed SD save or first tombstone | UI | NVS only | yes (conditional) |
| `wifi_cred` | 98 | ~6 | `hal_esp32.c:2004` | after association; 5 s retry | `wifi-settings` | SD `watch/wifi` + NVS mirror | yes |
| `bat_cal` | 4 | ~3 | `hal_esp32.c:314` | serial `batcal` | `cmd` | NVS | yes (rare) |
| `pedometer` | 24 | ~3 | `board.c:915` | 60 s if changed, only screen not ON; 5 s retry; at shutdown | `board-fs` | NVS | yes |
| `nosact` | 92 | ~5 | `svc.c:499` | >10 min after last save if dirty; rollover; 5 s retry | UI | NVS | yes |
| `nosalarm` | 164 | ~8 | `svc.c:728` | per edit / one-shot fire / phone sync, no retry | UI | NVS | yes |
| `nos_wc` | 6 | ~3 | `time_worldclock.c:60` | per favourite toggle, void | UI | NVS | yes |
| `sportsfood` | 8200 | ~262 | `sports.c:347,361` | per intake/undo, checked | UI | NVS | yes |
| `sportsmarks` | 1032 | ~35 | `sports.c:202` | per report/label, checked | UI | NVS | yes |
| `sportstune` | 20 | ~3 | `tuning.c:40` | per +/- tap, checked | UI | NVS | yes |
| `wake_cfg` | 48 | ~4 | `wake_service.c:63` | on setting change / learn done; 1 s retry from sync | UI | NVS | yes |
| `ext_sea` | 4 | ~3 | `ext_env.c:196` | per SET, void | UI | NVS | only with baro strap |
| `ext_mag` | 36 | ~4 | `ext_mag.c:260,294` | calibration end (void) / `ext mag map` (checked) | UI / `cmd` | NVS | only with mag strap |
| `cfg` | 40 | ~4 | `core/settings.c:74` | never in NOS (load only) | UI at boot | NVS | read-only legacy |
| `weather_loc` | 44 | ~4 | `weather_service.c:36` | no NOS caller (load only) | n/a | NVS | read-only legacy |
| `voice_chat` / `voice_hist` | 228 / ~3892 | ~10 / ~124 | `voice_chat.c:534,205` | on config / dirty history | UI | NVS | C only; stub on B |
| `alarms`, `pip_memory`, `pip_care_a`, `pip_care_b`, `lcd_gap` | 72, 20, 176, 176, 4 | 5, 3, 8, 8, 3 | legacy `core/apps`, `core/pip*`, `main.c` | never in NOS | n/a | NVS | dead; data may remain |

Non-`hal_store` NVS users on B: namespace `phy` (cal_version, cal_mac, cal_data 1904 B, about 66 entries, `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y`, `firmware/sdkconfig:1553`; `esp-idf/components/esp_phy/src/phy_init.c:731-770`). Wi-Fi config storage is RAM (`hal_esp32.c:2286`); NimBLE bond persistence is off (`CONFIG_BT_NIMBLE_NVS_PERSIST is not set`, `sdkconfig:738`). Plus the `wos` and `phy` namespace entries.

Routed to SD: only `nosprefs` (`/sdcard/watch/settings.{0,1}.bin`) and `wifi_cred` (`/sdcard/watch/wifi/settings.{0,1}.bin`). Everything else is NVS only. SD-only bulk data (journals, workouts, logs) uses no `hal_store`.

Consumers of the capacity gauge: `board.c:529` (`nvs_get_stats`), Settings > Modules storage page (`nos/apps/set_modules.c:1304-1314`, "SETTINGS STORE n% USED" = `used_entries/total_entries`), and serial `sd` (`nos_main.c:1787`).

## Persistence

- Authoritative SD: `nosprefs`, `wifi_cred`. NVS mirrors are best-effort. Failed SD saves stay dirty (prefs, wifi) with a 5 s retry.
- NVS-only data is lost if NVS is erased. `tools/flash_watch.py:33,150` and `:443-447` refuse flash ranges that overlap 0x9000..0xF000 and verify NVS is unchanged on recovery.
- **Capacity model** (derived from ESP-IDF source and the struct sizes above; no on-device measurement exists in the repo, `docs/` has no recorded `FLASH nvs=` figure, and the simulator reports a fixed 84/756, `sim/nos_hal_sim.c:449-450`):
  - 24 KB = 6 pages x 126 entries = 756 total. `PageManager::requestNewPage` needs a free page to relocate (`nvs_pagemanager.cpp:139-200`), and `available_entries = free - 126` (`:244-246`), so about 630 entries are usable (83% of the gauge's total).
  - Estimated live use on a fully exercised B watch: `wos` keys ~352 (of which `sportsfood` ~262, `sportsmarks` ~35), `cfg` 4, legacy leftovers ~24, `phy` ~66, namespace entries 2. Total roughly 450 of 756 (about 60% on the gauge, about 71% of usable).
  - `Storage::writeItem` writes a blob's new version first and erases the old one afterwards (`nvs_storage.cpp:~480-500`). Rewriting `sportsfood` therefore needs about 262 free entries on top of the existing 262. Free usable is about 630 - 450 = 180, which is less than 262.
  - If the model holds, the first food entry succeeds (no old copy) and later `sports_intake` / `sports_undo` calls return 0 ("SAVE FAILED"). Rewriting `sportsmarks` (35 entries) and all small keys still fits until the partition is essentially full.
  - Only measurement settles it: run serial `sd` and read `FLASH ... nvs=used/total` before and after an intake, and compare against 630 usable entries.
- Wear: `pedometer` rewrites a 24 B blob each minute the watch is off-screen and steps changed (up to about 1440 per day). Each rewrite consumes 3 entries until GC; the partition has no dedicated wear control beyond NVS page rotation. `nosact` is at most once per 10 min. Sports ledger writes burn about 262 entries per intake.
- **Failure behaviour when NVS is full or uninitialised**:

| Writer | Result on `ESP_ERR_NVS_NOT_ENOUGH_SPACE` (or init failure) |
|---|---|
| `sports_intake/undo/report`, `tuning_adjust` | returns 0, RAM rolled back, Energy shows SAVE FAILED |
| `nosprefs`, `wifi_cred` | SD result decides; NVS mirror failure ignored |
| `prefs_overlay` | returns 0 -> `overlay_checkpoint` fails -> `prefs_flush` returns 0, prefs stay dirty |
| `pedometer` | retry every 5 s; at shutdown blocks power-off/restart |
| `nosact` | dirty + 5 s retry, error counters never shown |
| `nosalarm` | traced only, no retry, silent divergence |
| `nos_wc`, `ext_sea`, `ext_mag` (calibration), `bat_cal` | void save, silent loss; UI reports success |
| `wake_cfg` | `wake_service_set` returns 0; Settings toasts "WRIST NOT SAVED: RETRY" at `set_settings.c:1840-1843`; `wake_wrist_sync` retries every 1 s indefinitely |
| NVS init `NO_FREE_PAGES` | logged only; all NVS loads return 0 and saves return 0 for the whole boot |

- Defaults and silent fallback: failed loads leave compile-time defaults (food ledger empty, alarms zero, activity zero).

## Interactions

- [[Storage and persistence]]: this note corrects its key list (add `wake_cfg`, `cfg`, `ext_sea`, `ext_mag`; mark `weather_loc` load-only; mark `voice_*` C-only). Its Gap "NVS 24 KB may overflow" is quantified here.
- [[Settings and developer]]: `nosprefs`, `prefs_overlay`, `wake_cfg` (Settings wrist/learn), Modules storage gauge.
- [[Sports and workouts]] and [[Calorie trace]]: `sportsfood`, `sportsmarks`, `sportstune`; SD journal replay keyed on persistent record IDs.
- [[Health and activity]]: `nosact`, `pedometer`.
- [[Services hub]]: `svc_tick` drives `nosact`, `nosalarm`; [[Utility apps]] (Alarms, World clock).
- [[Connectivity and voice]]: `wifi_cred`, `weather_loc`, `voice_*`.
- [[Power and battery]]: `bat_cal`, shutdown/sleep gating on `pedometer` and prefs; [[Engine and scheduling]] for task priorities; [[Hardware and drivers]] for strap calibration keys.
- [[Shell and faces]]: legacy `pip_*` face and `core/apps` are not in the NOS shell; see [[Gaps]] and [[OS overview]].

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| high | NVS capacity: `sportsfood` (8200 B, ~262 entries) cannot be rewritten while its old copy exists once PHY, legacy and other keys occupy ~450 of 756 entries (modelled, not measured). | `nos/sports.c:70,347,361`; `firmware/partitions.csv:5`; `nvs_storage.cpp:~480-500`; `nvs_pagemanager.cpp:244-246` | Second and later food/water entries may fail with SAVE FAILED, and the cause is invisible. Measure with serial `sd`. |
| high | `sportsfood` is the only large NVS blob; it is NVS-only and not SD-authoritative, unlike prefs. | `hal_esp32.c:236`; `docs/research/HEALTH_SYSTEM_ANALYSIS_20261021.json:1268` | Intake ledger depends on a nearly full partition; erasing NVS loses unexported entries. |
| high | A failed `pedometer` save blocks shutdown, so a full or uninitialised NVS can keep the watch from powering off or restarting while steps are dirty. | `board.c:903-918,1146-1152`; `nos_main.c:799-801` | User sees "shutdown blocked" style behaviour with no reason given. |
| medium | `nvs_flash_init` failure (`NO_FREE_PAGES`) is logged and ignored; boot continues with a dead store. | `hal_esp32.c:2316-2323` | All fallback loads and NVS-only saves silently fail; user settings revert to defaults for the session. |
| medium | Capacity gauge is sampled once at boot and divides by 756 (includes the GC reserve page), so it never shows the real ~83% ceiling and goes stale. | `board.c:517-531,923`; `set_modules.c:1307-1314`; `nvs_pagemanager.cpp:224-247` | Users and developers get no warning before saves start failing. |
| medium | `nosalarm` has no dirty flag or retry; failures are only traced. | `svc.c:721-730` | An alarm edit can be lost across reboot while the UI shows it set. |
| medium | `nosact` has no shutdown flush, so up to 10 min of activity history is lost on power-off/reset; its checkpoint error counters are never read. | `svc.c:496-509,558`; grep `checkpoint_*` in `nos/`, `firmware/main/` | Hourly step chart can lose the latest bars. |
| medium | Void `hal_store_save` callers (`nos_wc`, `ext_sea`, `ext_mag` calibration, `bat_cal`) lose data silently on failure, while the UI confirms success. | `time_worldclock.c:60`; `ext_env.c:196`; `out_baro.c:686-688`; `ext_mag.c:260`; `hal_esp32.c:314` | Barometer sea-level or compass calibration reverts after reboot with no warning. |
| medium | `wake_wrist_sync` retries every 1 s forever on save failure, so a full NVS means a flash write attempt per second. | `nos_main.c:939-950` | Extra flash wear and possible frame stalls while NVS is full. |
| medium | Synchronous NVS commits (and GC page erases) run inside UI frames for `nosact`, alarms, sports, tuning, `nos_wc`, `ext_*`, with no power-state gating, unlike prefs and pedometer. | `svc.c:1303-1318`; `sports.c:347`; `docs/OPTIMIZATIONS_DEVICE.md:28` | Possible hitch when a save lands during animation; duration not measured. |
| low | `weather_loc` has no setter in the NOS build; only the unbuilt legacy `main.c` can set it. | `main.c:776`; grep `hal_weather_set_location` | A B watch flashed fresh has no weather location source except phone-provided data. |
| low | `cfg` is read at boot (`settings_load`) but never rewritten in NOS; its stale brightness/timezone apply briefly before `prefs_load` overrides. | `core/settings.c:36-65`; `nos_main.c:1921-1923` | Cosmetic; orphaned data. |
| low | Legacy keys (`alarms`, `pip_memory`, `pip_care_a/b`, `cfg`, `lcd_gap`) stay in NVS on any watch that ran legacy firmware; nothing reclaims them, and NVS is never erased. | `hal_esp32.c:2318-2320`; `core/apps/app_alarm.c:106-108`; `face_pip.c:40-44` | ~24-28 entries of dead capacity; legacy alarms are not migrated to `nosalarm`. |
| low | The simulator's store has no capacity model (32 keys, unlimited bytes) and treats `nosprefs`/`wifi_cred` as ordinary keys. | `sim/nos_hal_sim.c:197-260` | NVS-full and SD-routing failures cannot be reproduced in the simulator. |
| low | `voice_chat_init` bypasses `hal_store_load` with a raw `nvs_open` (C only). | `voice_chat.c:98-103` | None on B; a second NVS read path to keep in sync on C. |
