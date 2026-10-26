---
title: "Storage and persistence"
tags: [nos, os-map]
updated: 2026-10-23
---

# Storage and persistence

## Summary

NOS persists through three media on the 1.43-B: the NVS partition (24 KB, namespace `wos`, blob API `hal_store_*`), the FAT SD card (`/sdcard`, 1-bit SDMMC), and RTC_NOINIT RAM (breadcrumbs that survive warm reset only). Only two keys, `nosprefs` and `wifi_cred`, are SD-authoritative (alternating checksummed `settings.0/1.bin`) with NVS as mirror/fallback ([[Settings and developer]]); every other `hal_store_*` key goes straight to NVS (`firmware/main/hal_esp32.c:236`). Bulk, append-style data (sports journals, workout checkpoints, IMU CSV, GPX tracks, diagnostics ring, WAV memos, minute logs) lives on SD only. A transfer protocol (`sd_xfer.c`) over USB serial or BLE GATT (`ble_xfer.c`) moves files to and from a PC/phone (`tools/sdsync.py`); `ota.c` handles firmware updates into two 0x780000 OTA slots (`firmware/partitions.csv`); `tools/flash_watch.py` is the checked flasher that checkpoints SD settings and refuses to touch the NVS range 0x9000..0xF000.

Legacy `main.c`, `wos.c`, `core/apps/*`, `core/pip*.c` and `core/settings.c` persistence are not part of the NOS build (note: `core/settings.c` is listed in scope but is legacy; NOS settings are `nos/prefs.c`). See [[Gaps]] and [[OS overview]].

## Components

### Preferences store (nos/prefs.c, nos/prefs.h)
- Purpose: RAM struct `nos_prefs_t` (52 B) is the single settings owner, with dirty flag and debounced save.
- Runs: main loop (`prefs_tick` at `nos/prefs.c:366`, called from `firmware/main/nos_main.c` loop ~2098/2110/2163); `prefs_flush` (331) at shutdown; command `prefs`/`sd` in cmd task (`nos_main.c:1620-1790`).
- Cadence: save after 2 s debounce, forced at 10 s max age, 5 s retry after failure; only inside a "persistence window" (screen off/AOD, or screen on after `frame_drain`) so SD I/O never lands mid-frame.
- Reads: `hal_store_load("nosprefs")` (`prefs.c:196`, legacy-size migration `prefs.c:203`), edit-mask + NVS `prefs_overlay` merge for card-absent boots, restore gate IDLE/FLUSHING/HELD/RELOAD (`prefs.c:303-316`) for raw `sdput!` restore of settings files.
- Writes: `prefs_save` (260) -> `hal_store_save_checked` -> SD banks + NVS mirror.
- Files: `nos/prefs.c:196,260,303,331,366`, `nos/prefs.h`.

### SD settings bank format (nos/prefs_disk.c, prefs_disk.h)
- Purpose: crash-safe settings file pair `/sdcard/watch/settings.0.bin` and `settings.1.bin` (wifi: `/sdcard/watch/wifi/`).
- Format: 280 B slot with FNV-1a checksum; alternates banks; writes, fsync, readback verify; skips write when content unchanged; never truncates an unreadable bank; status enum `prefs_disk_read_status`.
- Runs: in caller context (main loop for prefs, wifi-settings task for credentials), `nos/prefs_disk.c:180-209` for save.
- Files: `nos/prefs_disk.c:180-209`, `firmware/main/hal_esp32.c:236-282`.

### hal_store blob API and NVS (hal_esp32.c)
- Purpose: `hal_store_save/load/save_checked` over NVS namespace `wos` (`firmware/main/hal_esp32.c:176-282`); overlay slot `prefs_overlay` (208-219).
- Keys written (all `hal_store_*`): `nosprefs` (SD+NVS), `wifi_cred` (SD+NVS), `prefs_overlay`, `bat_cal` (`hal_esp32.c:314-318`), `pedometer` (`board.c:1069`, saved by `save_pedometer`, `board.c:905-918`), `nosact` (`nos/svc.c:565`), `nosalarm` (`nos/svc.c:735`), `nos_wc` (`nos/apps/time_worldclock.c:52,60`), `sportsfood`, `sportsmarks` (`nos/sports.c:310,313`), `sportstune` (`nos/tuning.c:18`), `voice_chat` (`voice_chat.c:111,535`), `voice_hist` (`voice_chat.c:206`), `weather_loc` (`weather_service.c:163`). Legacy-only: `lcd_gap` (`main.c:426`).
- Partition: nvs 24 KB at 0x9000 (`firmware/partitions.csv`, mirrored in `tools/flash_watch.py:33`).

### SD mount, lease and lock (board.c)
- Purpose: mount 1-bit SDMMC FAT (`firmware/main/board.c:558-612`), lease counter `board_sd_asset_acquire/release` (`s_asset_users`, 613-628), mutex `board_sd_asset_lock/unlock` (`s_sd_lock`), plus `hal_sports_sd_lock` (50 ms timed). Unmount (629) only try-locks, requires zero leases, sticky teardown error.
- Runs: task `board-fs` (prio 1, `board.c:919-970`), plus sensing in `board-sense` (prio 2). Mount at `board_init` (1052-1073); `board_prepare_sleep` (~1126).

### Board minute log and pedometer (board.c)
- Purpose: `log_sample` (`board.c:713-745`) appends minute logs under `/sdcard/watch/logs`; `save_pedometer` (905-918) persists step counter to NVS `pedometer` (hourly/quiet-point style, via `board_fs_task`).
- Runs: `board-fs` task; works with screen off.

### Sports journal (nos/sports.c)
- Purpose: 64 B records, per-day event journal and per-hour raw files on SD; food ledger and markers in NVS (`sportsfood`, `sportsmarks`); runs on `sports_sd` task (prio 1); see [[Sports and workouts]], [[Calorie trace]].
- Files: `nos/sports.c:310-313`, `nos/tuning.c:18`.

### Workout checkpoints (nos/workout.c)
- Purpose: alternating-bank `workouts.%d.bin` (4304 B, 12 history) at `nos/workout.c:523`; task `workout_sd` (prio 2).

### GPX track saver (nos/svc_track.c)
- Purpose: track points in RAM, written to `/sdcard/...` GPX on stop by one-shot `track_save` task (prio 1). Not saved incrementally.

### IMU recorder (nos/imu_rec.c)
- Purpose: CSV segments `/sdcard/imu/%04d.csv` (`imu_rec.c:230,278,314,334`), `.link` intent banks (286); IMU worker task.

### Activity and alarm service state (nos/svc.c)
- `nosact` (activity history store, `svc.c:395-580`, load 565) and `nosalarm` (~690-740) in NVS; flush at quiet points. Whether `nosact` is flushed in the shutdown path was not verified.

### World clock favourites (nos/apps/time_worldclock.c)
- `nos_wc` NVS key, saved on edit (`:60`).

### Voice chat config and history (voice_chat.c, nos/voice_history.c)
- `voice_chat` config and `voice_hist` compact history blobs in NVS (`voice_chat.c:98,111,205,534`, `nos/voice_history.c`). See [[Connectivity and voice]]. 1.43-B has no built-in audio.

### Voice memos (nos/apps/con_memo.c, ext_audio)
- WAV memos written as `.part` then renamed (`firmware/main/ext/ext_audio.c`, `ext_audio_files.inc`); `memo_scan` task (prio 1) enumerates. The memo directory is never created by the code reviewed (see Gaps).

### Weather location (weather_service.c)
- `weather_loc` NVS key (`weather_service.c:163`); `weather` task prio 2.

### Diagnostics log (nos/diag_log.c, diag_log.h)
- Purpose: 44 B `DLG1` records in a ring of 8 x 1 MiB files `diag-%02u.bin` (`diag_log.c:395`) on SD; PSRAM queues; RTC_NOINIT breadcrumbs and fault ledger (survive warm reset only); drained by `diag_log_tick` from main loop. Complemented by coredump partition and RTC heap-failure history (`firmware/main/nos_heap_diag.c`). Exported with `tools/diag_export.py`; `diag log` command (`nos_main.c:1204`).

### SD transfer service (firmware/main/sd_xfer.c)
- Purpose: line protocol `sdls/sdcrc/sdput/sdput!/sdget/sdget2/sdrm`; staged `.part`, journal `.sdxfer.txn`, `.sdxfer-old` backup, CRC32, ACK pacing, protected paths, path builder `sd_xfer.c:93-98`; OTA mutation scope (`ota_external_activity_begin`). Runs in `cmd` task (prio 4, 6144 B) for USB, in the BLE callback context for GATT.

### BLE transfer (firmware/main/ble_xfer.c)
- Same protocol over GATT chars `7f5e0002-...` (control) / `7f5e0003-...` (data) (`tools/sdsync.py:23-24`); one connection at a time; advertised name "Bangle.js NOS".

### OTA (firmware/main/ota.c)
- Purpose: dual-slot OTA. Session token (600 s), writer claim with runtime-admission policy, `esp_ota_end` SHA-256 verification, rollback enabled, image marked valid after 90 rendered frames. Web upload (`hal_esp32.c:1830,1892,1931`), `ota_url` task (prio 5, 8 KB, `ota.c:387`), `ota-upload` task. `OTA_SD_PATH /sdcard/update.bin` and `OTA_SD_DONE` are defined (`ota.c:31`) but see Gaps.

### Asset reader (hal_esp32.c, core asset job)
- `asset-read` task (prio 1, `hal_esp32.c:2415-2470`) loads textures such as `/sdcard/sky/*.rgb` (`nos/faces/orrery_card.c:776`) under a mount lease.

### Host tools
- `tools/sdsync.py`: mirror/ls/get/pull/rm via USB (`--port`, finds serial `28:84:85:B0:8B:30`) or `--ble`; size+CRC32 skip of unchanged files.
- `tools/flash_watch.py`: checked flasher; checkpoints SD settings, verifies erase ranges do not overlap NVS (`:33,150`), verifies NVS unchanged on recovery (`:443-447`).
- `tools/diag_export.py`: pulls diag ring. `tools/backup_flash.py`, `tools/flash_layout.py`.

## How it runs

- Main loop (core 0 style loop in `nos_main.c`) owns prefs debounce and diag drain; it never blocks on SD beyond lock timeouts, and prefs only flush inside a persistence window.
- SD-touching tasks: `board-fs` (1), `sports_sd` (1), `workout_sd` (2), `track_save` (1, one-shot), `asset-read` (1), `memo_scan` (1), `extf` (0), `exta` (3), IMU worker. All cooperate through the mount lease and SD mutex; unmount waits for zero leases.
- `cmd` task (prio 4) serves serial commands and `sdput` transfers; `flush`(prio 4, core 1) and `touch` (prio 5, core 1) are display/input tasks that must not wait on SD.
- Screen-off: preference saves, minute logs, pedometer, sports and diag drains all continue with the screen off (persistence window opens in AOD/off). Deep-sleep/shutdown path (`nos_main.c:759-830`) calls `prefs_flush`, reports blocking reasons (`ota_busy`, `imu_owner`, `recording_pending`, `audio_owner`; 830) and `board_prepare_sleep`.
- Boot order (`nos_main.c:1899-1950`): NVS init, SD mount, prefs load (SD banks preferred, NVS fallback + overlay), pedometer load, services.

## Data in / data out

| Direction | Source or sink | Data | Mechanism |
|---|---|---|---|
| in | PC (USB serial) | files, settings restore, OTA image | `sdput`/`sdput!`, `ota` commands via cmd task |
| in | Phone/PC (BLE) | same transfer protocol | GATT control/data chars |
| in | Wi-Fi | OTA by URL or web upload | `ota_url` / web handler |
| out | PC/phone | files, diag ring, logs, GPX, IMU CSV | `sdget`/`sdget2`, `sdls` |
| in | [[Settings and developer]] | prefs edits | `prefs_*` setters set dirty |
| in | [[Health and activity]] | steps, activity, alarms | `pedometer`, `nosact`, `nosalarm` |
| in | [[Sports and workouts]] | sample journals, workouts, tracks | SD tasks |
| out | [[Rendering pipeline]] / faces | textures from `/sdcard/sky` | asset-read lease |
| in | [[Power and battery]] | `bat_cal` calibration | NVS |

## Persistence

| Item | Medium | Survives reboot | Notes |
|---|---|---|---|
| nosprefs | SD settings.0/1.bin + NVS | yes | SD authoritative; NVS fallback; failed SD save stays dirty |
| wifi_cred | SD watch/wifi + NVS | yes | plaintext, not recorded here |
| prefs_overlay | NVS | yes | card-absent edit merge |
| bat_cal, pedometer, nosact, nosalarm, nos_wc, sportsfood, sportsmarks, sportstune, voice_chat, voice_hist, weather_loc | NVS | yes | 24 KB total budget |
| sports journals, workouts.N.bin, imu/, GPX, logs, memos, diag ring | SD | yes | absent when card missing |
| diag breadcrumbs, fault ledger | RTC_NOINIT | warm reset only | lost on power loss |
| coredump | flash partition | yes | |
| firmware | 2 OTA slots 0x780000 from 0x20000 | yes | rollback on |
| SD/RAM-only | BLE bonds, notifications, GB state, weather, music state | no | see Gaps |

## Interactions

- [[Settings and developer]]: all prefs edits and the `prefs`/`sd` developer commands.
- [[Engine and scheduling]]: persistence windows tied to `frame_drain` and screen state.
- [[Power and battery]]: shutdown flush, `board_prepare_sleep`, `bat_cal`.
- [[Hardware and drivers]]: SD host, NVS, flash partitions.
- [[Health and activity]], [[Sports and workouts]], [[Calorie trace]]: journals and checkpoints.
- [[Connectivity and voice]]: BLE xfer, OTA, Wi-Fi credentials, voice chat/history.
- [[Services hub]]: `nosact`, `nosalarm`.
- [[Shell and faces]], [[Utility apps]]: world clock favourites, memos, Orrery textures.
- [[Gaps]], [[OS overview]].

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| high | NVS 24 KB may overflow: many separate blobs including `voice_hist`, `nosact` (history) and overlay | `firmware/partitions.csv`, key list above | silent failure to save settings fallback or history; measure with `sd` command (estimate, unverified) |
| high | BLE/USB SD transfer and OTA appear unauthenticated beyond protected paths and OTA token | `firmware/main/sd_xfer.c`, `ble_xfer.c`, `ota.c` | nearby BLE client can read/overwrite SD files including settings and credentials |
| medium | Wi-Fi credentials stored in plaintext on SD and NVS | `hal_esp32.c:243,2004` | card removal exposes network password |
| medium | Memo directory not created by reviewed code | `nos/apps/con_memo.c`, `ext_audio.c` | memos may fail to save on a fresh card |
| medium | GPX track saved only on stop; RAM-only until then | `nos/svc_track.c` | reboot or crash mid-activity loses the track |
| medium | SD OTA path defined but unwired | `firmware/main/ota.c:31` | no offline update from card |
| medium | Unbounded SD growth: IMU CSV, minute logs, sports per-day files, GPX, memos have no rotation (only diag ring and 12 workouts are bounded) | `nos/imu_rec.c`, `board.c:713-745`, `nos/sports.c` | card fills over time |
| medium | BLE bonds and several states are RAM-only | no `hal_store` use in `gb.c`/`notif.c` | re-pair/lose notifications after reboot |
| low | RTC_NOINIT diag breadcrumbs lost on power loss | `nos/diag_log.c` | crash cause unknown after brownout |
| low | `nosact` shutdown flush not verified | `nos/svc.c:565`, `nos_main.c:759-830` | up to one flush interval of activity may be lost |
| low | `core/settings.c` is legacy and unused by NOS | build scope | confusion in docs |
