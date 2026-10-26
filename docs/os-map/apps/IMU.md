---
title: "IMU"
id: app-imu
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# IMU

Source: `nos/apps/out_imu.c` (195 lines), with the recorder in `nos/imu_rec.c` (471 lines), `nos/imu_backend.h`, `firmware/main/imu_hal.c` (114 lines) and the board glue in `firmware/main/board.c`. Read-only source review; nothing built, flashed or run on a board. Evidence is `repo-relative/path:line`. Where `docs/IMU_APP.md` disagrees with the code, the code is cited and the doc is named.

## Summary

IMU is the NOS raw-motion screen. It is a three-tab pager, LIVE / RECORD / FILES (`nos/apps/out_imu.c:158-163`). LIVE shows the QMI8658 accelerometer and gyro. RECORD starts and stops a 25, 50 or 100 Hz CSV recording on the SD card. FILES browses, reads and deletes those CSV files.

The screen owns no sensor, no storage and no settings. It reads snapshots and sends asynchronous commands. The recorder is a separate worker (`nos/imu_rec.c`) with its own tasks, and recording continues with the display off or after the app is left (`docs/IMU_APP.md:40-41`; no screen gate in `firmware/main/imu_hal.c:25-53`).

Only the 1.43-B has the IMU wired (`firmware/main/pins.h:61` sets `BOARD_HAS_IMU 1` on the B block; the C block has none, `pins.h:27`). On C the LIVE tab shows "IMU UNAVAILABLE" (`out_imu.c:118`), and recording fails on the missing card (C has `BOARD_HAS_SD 0`, `pins.h:29`).

Status is **partial**. Source coverage is complete. Board-level IMU samples were reported valid on the connected hardware on 2026-09-12 (`docs/VALIDATION.md:10, 66-76`), but that entry does not name the IMU screen or the recorder. `docs/IMU_APP.md:135-150` lists the recorder's device checks as still pending, and the repo holds no device result for them. Several gaps below were traced from source and not reproduced on a board.

Related notes: [[Hardware and drivers]] (QMI8658, SD, board-sense), [[Storage and persistence]] (SD layout, intent banks), [[Engine and scheduling]] (tick and task model), [[Shell and faces]] (launcher, Home recording chip), [[Services hub]], [[Power and battery]] (power guard, low battery), [[Health and activity]] and [[Sports and workouts]] (shared IMU feed), [[Jobs]] (BG_IMU_REC row).

## Features

- **LIVE (tab 0).** Hero "|A| G" is the magnitude of the accel vector, drawn with the dot font (`out_imu.c:55, 57, 120-121`). Two text readouts show X/Y/Z for accel in g and gyro in deg/s (`out_imu.c:58-59, 126, 129`). A TILT bubble is driven by ax and ay (`out_imu.c:60-61, 122-125`). Two strip traces, each with three channels, show accel (12 px per g) and gyro (0.2 px per deg/s), clipped to ±24 px (`out_imu.c:62-63, 93-111`). The span label "ACCEL +/-2G / n.nS" is measured from the stamps, not assumed (`out_imu.c:67-68`). The ring holds 64 samples (`out_imu.c:12`).
- **Sampling cadence.** LIVE samples at most every 50 ms, and only while the LIVE tab is shown (`out_imu.c:51`). On a 30 fps frame cadence that is about every other frame, so about 15 Hz, and the 64-sample window is about 4.3 s (`docs/IMU_APP.md:8`).
- **Invalid data.** "IMU UNAVAILABLE" appears when the board has no fresh sample (`out_imu.c:53-54, 118`). Non-finite values are rejected (`out_imu.c:56`).
- **RECORD (tab 1).** Three rate chips, 25 / 50 / 100 Hz. The default is 50 Hz (`out_imu.c:43, 140-141, 178-180`). One button: START RECORDING, STARTING, STOP + SAVE, SAVING, or NEW RECORDING (`out_imu.c:145`). A red REC dot shows while busy (`out_imu.c:116`). While busy or in review the page shows elapsed time, SAMPLES, B WRITTEN, DROP/MISS/ERR and the current CSV name (`out_imu.c:135-137`). An error replaces the name with the error text (`out_imu.c:138`). After a recording ends the summary stays until NEW RECORDING (`out_imu.c:33, 136, 182`).
- **FILES (tab 2).** On entry the newest file is requested (`out_imu.c:45`). "<" selects an older file and ">" a newer one (`out_imu.c:187-188`). The page shows name, duration (the last CSV `ms` value) and size in bytes (`out_imu.c:150-152`, `nos/imu_rec.c:336-352`). DELETE needs two taps, with "TAP AGAIN TO DELETE" as the prompt (`out_imu.c:153-155, 189-190`). Browse and delete are blocked while recording (`out_imu.c:147`, `nos/imu_rec.c:172-173`).
- **Feedback.** Tab change gives FB_TICK. An accepted command gives FB_TAP. A refused command gives FB_ERROR and the toast "IMU BUSY / UNAVAILABLE" (`out_imu.c:174, 179, 182, 192-193`).

## Reads

Service getters: none. `nos/apps/out_imu.c` makes no `svc_*` call.

Prefs and NVS: none. `out_imu.c` and `imu_rec.c` contain no `prefs_*`, `nos_prefs.*` or `hal_store_*` call. The rate chip is RAM only (`out_imu.c:43`).

HAL and driver calls the screen makes:
- `hal_imu_read()` (`out_imu.c:53`) → `firmware/main/hal_esp32.c:477` → `board_imu_read()` (`firmware/main/board.c:388-396`). This returns the board's cached sample `s_sample`, which is **filtered** (see Gaps). The sample is valid only if it is under 250 ms old (`board.c:393`).
- `hal_imu_record_get()` (`out_imu.c:32`) → `nos/imu_rec.c:163-164`. This returns a snapshot of `status`: state, hz, elapsed_ms, samples, dropped, missed, errors, bytes, name[16] and message[48].
- `hal_imu_file_get()` (`out_imu.c:32`) → `nos/imu_rec.c:165-166`. This returns a snapshot of `file_status`: busy, valid, id, duration_ms, bytes, name, message.
- `hal_millis()` (`out_imu.c:51-52`) for the 50 ms gate.
- `nos_mem_alloc(sizeof *s, "imu.ui")` (`out_imu.c:43`) → `nos/psram.c:29`. The block is 1,336 bytes (`docs/IMU_APP.md:90`).
- Drawing and feedback helpers only: `ui_*`, `gfx_*`, `dot_*`, `gfx_hash_u`, `gfx_rec_cmd` (`out_imu.c:75, 95`), `nos_feedback`, `nos_toast` (`nos/nos.h:201, 205`).

## Writes

RAM only in the screen. The `imu_ui` block `s` holds the page, rate, valid, confirm, review, ring and trace arrays and text buffers (`out_imu.c:13-23`). `s` is never freed (`out_imu.c:43`).

Commands to the recorder. Each is asynchronous and returns 0 if refused.
- `hal_imu_record_start(hz)` on START (`out_imu.c:184`) → `nos/imu_rec.c:184-185`. It is refused unless `nos_runtime_recording_allowed()` holds, which means no OTA and no shutdown pending (`nos/runtime_admission.h:8-9`). The worker then creates `imu/NNNN.csv` and `.link` and saves the intent bank (see Persistence).
- `hal_imu_record_stop()` on STOP (`out_imu.c:184`) → `imu_rec.c:186`. It is accepted only in RECORDING or ERROR (`imu_rec.c:172-174`). The screen never sends STOP while in ERROR (see Gaps).
- `hal_imu_files_request(id, dir)` on FILES entry and on "<" / ">" (`out_imu.c:45, 173, 187-188`) → `imu_rec.c:306-355`. This is a read-only scan.
- `hal_imu_file_delete(id)` on the confirmed DELETE (`out_imu.c:190`) → `imu_rec.c:314-315`. It calls `remove()` on `/sdcard/imu/NNNN.csv` only.
- Haptic and toast only: `nos_feedback`, `nos_toast`.

Not written: no prefs, no NVS, no svc mutator, no phone message.

## Background behaviour

**Screen not on top.** The screen's `tick`, `draw` and `input` run only while it is the top screen (`nos/nos.c:2104-2108`). `scr_imu` has flags 0, so it does not hold the display awake (`out_imu.c:195`, `nos/nos.h:64`). While hidden, the LIVE values and the RECORD counters are not refreshed. `enter()` refreshes them on the next entry (`out_imu.c:41-46`).

**Recording, independent of the screen.** Once started, the recorder keeps working through display sleep, AOD and leaving the app:
1. Timer `imu-rate` is an esp_timer with period 1e6/hz µs: 40 ms at 25 Hz, 20 ms at 50 Hz, 10 ms at 100 Hz (`firmware/main/imu_hal.c:98-104`).
2. Task `imu-sample`: priority 3, 4,096-byte stack from PSRAM, `tskNO_AFFINITY`, created at `imu_hal.c:81` and run in `sample_task` (`imu_hal.c:25-44`). On each timer notify it reads the QMI8658 12-byte block at register 0x35 (`firmware/main/board.c:398-407`), timestamps it before the I2C read (`imu_hal.c:38`), and pushes it into a 512-entry ring (`nos/imu_rec.c:35, 132-143`). Missed deadlines count as MISS (`imu_hal.c:35`). Failed reads count as ERR (`imu_hal.c:40`). A full ring counts as DROP (`imu_rec.c:136`).
3. Task `imu-write`: priority 1, 6,144-byte stack from PSRAM (`imu_hal.c:45-53, 89`). It loops `imu_worker_step()` and waits 20 ms while RECORDING or STOPPING, and 1,000 ms otherwise (`imu_hal.c:51`). Each step drains up to 512 points (`imu_rec.c:370-375, 410`), formats CSV rows (`imu_rec.c:357-369`), and flushes the 4 KiB batch when it is full or once a second (`imu_rec.c:92, 205-212, 429`). Every flush is fsynced (`imu_rec.c:43, 210`).
4. Segment rollover at `IMU_SEGMENT_BYTES` = 4 MiB (`imu_rec.c:37, 417-428`). The worker closes the file and opens the next numbered one.
5. SD lease. The worker takes the board SD asset lease before each step and keeps it while a CSV is open (`imu_rec.c:461-470`). `board_sd_unmount()` refuses while the recorder is non-idle (`firmware/main/board.c:634-635`) or any asset lease is held (`board.c:643-646`).
6. Background jobs. `background_tick()` runs at most once a second (`nos/background.c:43-45`). It publishes BG_IMU_REC with samples, elapsed time and missed count (`background.c:146-153`). The Jobs row for it opens this screen (`nos/apps/sys_background.c:213`).
7. Power policy. The screen-off loop runs at 50 Hz with `nos_bg_tick` at 1 Hz (`firmware/main/nos_main.c:2090-2095`). It does not pause the recorder. Deep sleep, power-off and restart are refused while the recorder is non-idle or any intent is active (`nos_main.c:731-740`). The refusal toast is "STOP IMU FIRST" (`nos_main.c:756-757`). OTA admission in `ota_poll()` uses the same recorder-busy and intent-pending test (`firmware/main/ota.c:53-61`). The base IMU is put to sleep only after both workers park (`firmware/main/board.c:1158-1163`).
8. Reboot. Boot reads both intent banks (`imu_rec.c:57-77`) and sets `boot_resume` when the saved intent is active (`imu_rec.c:76`). The worker then opens a new CSV and resumes, with no user action (`imu_rec.c:402-404`, `:293`, `:301` "RESUMED / NEW SEGMENT").

**Board sense, always running (not this app).** Task `board-sense` is priority 2, 4,096-byte PSRAM stack, with a 20 ms cycle that includes two knock polls (`firmware/main/board.c:1036, 1023-1025`). It samples the IMU at `board.c:984-986` and refreshes the cached `s_sample` that LIVE reads. The same samples feed the sports, knock and pedometer paths (`board.c:298-314`).

## Persistence

SD card only. `hal_sd_root()` returns `/sdcard` (`firmware/main/hal_esp32.c:1592`).

| Path | Contents | Written by | Read by |
|---|---|---|---|
| `/sdcard/imu/NNNN.csv`, NNNN 0000–9999 | Header `ms,utc_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,a_g` (`imu_rec.c:296`). Rows `%u,%llu,%.5f,...` (`imu_rec.c:361-363`). The name is the first free number from previous+1, created with O_EXCL (`imu_rec.c:276-282`) | `begin_record` (`imu_rec.c:250-302`), `write_point` (`imu_rec.c:357-369`) | `browse` reads the last 4,095 bytes (`imu_rec.c:306-355`) |
| `/sdcard/imu/NNNN.link` | `previous=`, `reason=` (`reset` or `start/rollover`), `previous_bytes`, `previous_valid_bytes`, `previous_missing` (`imu_rec.c:286-289`) | `begin_record` | Not read by firmware or the screen |
| `/sdcard/imu/intent.0.bin`, `intent.1.bin` | 24-byte intent: magic, seq, active, hz, file, FNV-1a sum (`imu_rec.c:47-56`). Two alternating banks, each written, fsynced and read back (`imu_rec.c:78-91`) | `intent_save` (`imu_rec.c:78-91`) | `intent_load` at boot and on retry (`imu_rec.c:57-77`) |

Protections. `imu/intent.*` is a protected path for raw SD restore (`firmware/main/sd_xfer.c:296`). `live_path()` treats everything under `imu/` as live while the worker is pending, and the open CSV as live while recording (`sd_xfer.c:299-309`). The OTA start path refuses on the same recorder-busy test (`firmware/main/ota.c:53-61`). I did not read the restore caller that consumes `live_path()`, so the exact refusal behaviour there is unverified.

Sizes. A segment rolls over at 4 MiB (`imu_rec.c:37`). At the row format above, a row is about 70 to 80 bytes. That is an estimate from the format, not a measurement. So a segment holds about 9 to 10 minutes of 100 Hz data. The ring is 512 × 40 bytes = 20,480 bytes (`docs/IMU_APP.md:92`).

Limits. There is no quota, no rotation, no size or time cap, and no free-space check. `begin_record` checks only mount, PSRAM and directory (`imu_rec.c:259-266`). The name space is 10,000 numbers (`imu_rec.c:276-282`).

Not persisted: the rate chip, the current page, review state, and the trace buffers. These reset at reboot.

## Navigation

Opens: nothing. `out_imu.c` calls neither `nos_open` nor `nos_push`.

Opened from:
- Launcher app list, entry "IMU" with DOT_ICON_GAUGE (`nos/apps/registry.c:27`). The last app opened is remembered and a double press reopens it (`registry.c:40-53`).
- Jobs screen rows BG_MOTION and BG_IMU_REC (`nos/apps/sys_background.c:213`).
- Home and Control recording chip, kind 3, shown while RECORDING, STARTING or STOPPING (`nos/apps/sys_home.c:158-161`, `:174`). The chip priority is memo, then IMU, then workout, then nav track (`sys_home.c:150-173`).
- Serial command `open imu` (`firmware/main/nos_main.c:1557-1558`), backed by the name table at `nos_main.c:924-937`.

Internal: the three tabs are a bottom bar at y 378–466 (`out_imu.c:158-163, 170-175`). The cancel gesture clears the DELETE confirm state (`out_imu.c:168`).

## Gaps

Severity is an estimate. Each item names its evidence.

1. **[high] A failed or interrupted recording leaves the recorder in ERROR with intent still active. Power-off and restart are refused, and the screen offers only START.** `finish()` never clears `intent.active` (`nos/imu_rec.c:213-225`). Only a STOP that reaches the worker clears it (`imu_rec.c:395, 413`). ERROR is not a busy state, so the button sends START, not STOP (`out_imu.c:25, 145, 184`). `imu_worker_pending()` counts `intent.active` (`imu_rec.c:435-436`), and the power guard uses it (`firmware/main/nos_main.c:739`). So the power sheet and restart return "STOP IMU FIRST" (`nos_main.c:756-757`). Triggers traced: SD removal mid-recording (`imu_rec.c:409`, lease failure at `:463`), a failed SD write, or a START while the IMU is not ready (`imu_rec.c:299`). The state clears only when the card returns and the retry starts a new segment, and then the user can press STOP. A permanent card fault leaves no UI path to clear it. Not reproduced on hardware.

2. **[medium] A failed start retries every ~5 s, creating a new header-only CSV and link each time, and recording can start by itself.** A failed `hal_imu_stream_start` calls `finish("IMU / PSRAM UNAVAILABLE")` (`imu_rec.c:299`). `imu_backend_start` requires `board_imu_available()` (`firmware/main/imu_hal.c:100`, `firmware/main/board.c:397`). `device_result` clears that flag after three consecutive IMU I2C errors (`board.c:200-210`), and `retry_devices` restores it later (`board.c:437-485`). While `intent.active` is set, the worker calls `begin_record` every 5 s (`imu_rec.c:402-404`). Each attempt creates a new `NNNN.csv` and `NNNN.link` (`imu_rec.c:276-292`). When the IMU returns, the retry records automatically. A header or SD write failure takes the same path (`imu_rec.c:298, 365, 429`). No test string covers "PSRAM UNAVAILABLE" (grep finds only the source line). Not reproduced on hardware.

3. **[medium] Elapsed time, SAMPLES, B WRITTEN and DROP/MISS/ERR restart at every 4 MiB segment.** `begin_record` zeroes the writer and stream counters (`imu_rec.c:258-259`). `hal_imu_stream_start` zeroes `start_ms` (`imu_rec.c:120-121`). `publish` takes elapsed time from `start_ms` (`imu_rec.c:198`) and counts rows and bytes from the per-segment writer (`imu_rec.c:196`). The rollover runs at `imu_rec.c:417-427`. The Home chip "IMU mm:ss" shows the same per-segment elapsed value (`nos/apps/sys_home.c:158-161`). `docs/IMU_APP.md:66` says counters are "session state". The code does not match that. A long recording looks restarted, and loss counts are not session totals.

4. **[medium] The FILES tab hides the segment chain.** One recording longer than 4 MiB is several unrelated `NNNN.csv` entries. The `.link` chain (`imu_rec.c:286-289`) is never shown. The tab shows one file at a time with name, duration and size only (`out_imu.c:150-152`). There is no preview, no summary (rate, sample count, min/max, gaps) and no on-watch export. Host download exists: `tools/sdsync.py --get` and `--pull` (`tools/sdsync.py:8-9, 345-346`), and firmware `sdget` and `sdget2` (`firmware/main/sd_xfer.c:14-16, 538-540`). `docs/IMU_APP.md:79-82` says sdsync "has no download command yet". That line is stale.

5. **[medium] No low-battery stop. Recording continues with the display off until the battery is empty.** Low battery moves the device to display-off only (`firmware/main/nos_main.c:2036`). The recorder is stopped only by the screen (`out_imu.c:184`) and by a simulator command (`sim/nos_sim.c:1037`). The power guard blocks power-off while recording (`nos_main.c:739`). The only sign of a running recording while the screen is off is the Home chip.

6. **[low] Newest-first default may open an older file.** A fresh START sets `intent.file=10000` (`imu_rec.c:401`). `previous_tail` then finds no predecessor (`imu_rec.c:229`), and the number search starts at 0000 (`imu_rec.c:270, 276-277`). After deletions, the new recording can take a lower number than existing files. FILES opens on the highest number, which can be an older recording (`imu_rec.c:322-333`, `docs/IMU_APP.md:17`).

7. **[low] Delete leaves `NNNN.link` behind.** `remove()` targets only the CSV (`imu_rec.c:314-315`). No other `remove(` or `unlink(` in `nos/` or `firmware/` touches `.link` files. The link is overwritten only if the number is reused (`imu_rec.c:287`). This is clutter in the 10,000-name space.

8. **[low] Without a card the recorder reports ERROR every second while idle.** The writer wakes every second when idle (`firmware/main/imu_hal.c:51`). The lease attempt fails with no card, and `imu_worker_step` publishes ERROR "NO SD CARD / RESUME WAIT" (`imu_rec.c:463`, `firmware/main/board.c:613-618`). That text shows on RECORD (`out_imu.c:142`). The Jobs row maps ERROR to BG_ERROR (`nos/background.c:149-150`). Nothing was requested, so the status is misleading.

9. **[low] Scale labels read like sensor range.** "ACCEL +/-2G" (`out_imu.c:68`) and "GYRO +/-120 DPS" (`out_imu.c:132`) describe where the trace clips (12 px/g and 0.2 px per deg/s, clamped to ±24 px, `out_imu.c:62-63, 93-111`). The sensor is configured for ±8 g and ±512 deg/s (`firmware/main/board.c:425-428`, comment at `firmware/main/board_protocol.c:263-265`). Readouts are not clipped.

10. **[low] LIVE shows the filtered board sample, not raw data.** `board.c:291-313` applies a single-pole filter at about 3 Hz (`k = dt/(0.053+dt)`, `board.c:305`). LIVE reads that value (`board.c:392`). Knocks and steps are attenuated in the traces. The recorder's CSV is raw (`imu_hal.c:38-39`). `docs/IMU_APP.md:8-9` documents this, but a "live motion" view can miss short events.

11. **[low] Axes are in sensor frame, not calibrated, and not mapped to the rotated panel.** The tilt and traces use raw `ax`/`ay` directly (`out_imu.c:58-61`). The B panel is rotated in software (`CLAUDE.md`, board mapping), and the screen has no IMU transform. `docs/VALIDATION.md:76, 101-103` lists gyro bias and IMU axis alignment as still to be checked.

12. **[low] Small UX traps.** "NEW RECORDING" takes two taps. The first only clears the review (`out_imu.c:145, 181-182`). The rate choice resets to 50 Hz at reboot (`out_imu.c:43`). Neither is persisted.

13. **[low] Power and CPU cost of recording is unmeasured.** `docs/IMU_APP.md:111-113` gives read and write rates (up to 100 reads per second, about 7 to 10 KB per second at 100 Hz) and disclaims any CPU claim. The device checks are pending (`docs/IMU_APP.md:138-150`), so the repo has no device result for the recorder.

## Notes on doc drift

- `docs/IMU_APP.md:66` ("counters are session state") conflicts with the per-segment reset (Gaps 3).
- `docs/IMU_APP.md:79-82` ("no download command") conflicts with `tools/sdsync.py:8-9` and `firmware/main/sd_xfer.c:14-16`.
- `docs/IMU_APP.md:17` (newest first) assumes monotonic numbering that the START path breaks after deletions (Gap 6).
- `firmware/main/board.c:291` says the IMU runs at 500 Hz with the accel low-pass opened to about 67 Hz. `docs/IMU_APP.md:34` says the chip "retains its existing ... hardware filters". Those two claims conflict. The register bit meanings were not checked against the QMI8658 datasheet.
