---
title: "USB command to handler map and host protocol edge cases"
tags: [nos, os-map]
updated: 2026-10-23
---

# USB command to handler map and host protocol edge cases

## Summary

The 1.43-B NOS firmware has one serial command surface (USB-Serial-JTAG, 115200 irrelevant, native USB) and one narrower BLE surface. Everything is parsed by a single FreeRTOS task, `cmd` (prio 4, unpinned, 6144 B stack, `firmware/main/nos_main.c:1891`). It splits traffic three ways:

1. **Framing rejects** (control bytes, >191 bytes, NUL barrier) answered inline with `ERR command input ... DONE` (`nos_main.c:866-881`).
2. **SD transfer verbs** (`sdfeatures sdls sdget sdget2 sdput sdput! sdcrc sdrm`) executed directly inside `cmd` under `s_host_reply_lock` (`nos_main.c:901-909`, `nos/host_request.c:20-26`).
3. **Everything else** copied into queue `s_cmd_q` (depth 4 x 201 B) and executed by `cmd_exec` on the UI (`app_main`, core 0, prio 1) task at the top of each loop iteration (`nos_main.c:1992-2005`).

BLE ("NOS XFR" GATT service) feeds only `sd_xfer_exec`; no `req` envelope, no UI commands. No command is gated by Developer Tools; the only run-time gates are the OTA/shutdown-phase-3 read-only allowlist, per-command `capture_active`/`touch_active` refusals, board capability bits (`ext`), and the C-board build flag (`chat`). `usb_guard` is a diagnostic override of `usb_serial_jtag_is_connected`, not an access control.

About 130 distinct verbs exist (table below). Edge cases that matter: unauthenticated BLE file read/write/delete including Wi-Fi credential files; silent no-reply for malformed SD verbs over USB (envelope still says `ok=1`); UI input is frozen while an SD mutation holds the OTA scope; `sdrm` can delete transaction/staging files; `flash_watch.py` never checks which board it is flashing.

## Components

### Serial framing parser (nos/command_input.c, nos/command_input.h)
- Purpose: byte-at-a-time LF framing with bounded capture. `COMMAND_INPUT_CAP 192` (`command_input.h:4`) so at most 191 text bytes + NUL; `_Static_assert` at `nos_main.c:885` ties it to `cmd_line.text[192]`.
- Behaviour (`command_input.c:7-27`):
  - `\n` ends the line (not counted). Result is `REJECTED` if any error was recorded, `READY` if at least one byte other than space/tab was kept, else `EMPTY` (blank or whitespace-only lines produce no output at all, no DONE).
  - `\r` is held as `pending_cr`; CR immediately before LF is tolerated and dropped, CR followed by anything else is `BAD_CONTROL`.
  - Bytes `<32` except TAB, and 127, are `BAD_CONTROL`; NUL is additionally counted (`nul_count`, first/last offset). UTF-8 high bytes pass through.
  - Overflow (`used >= 191`) records `TOO_LONG` (first_bad=0); the rest of the line is swallowed until LF, then rejected. No truncated command is ever executed.
  - Leading/trailing spaces are NOT trimmed, so `" board"` and `"board "` reach `cmd_exec` and hit `ERR unknown`.
  - There is no timeout and no reset on idle or reconnect: a partial line stays in the buffer forever and is concatenated with the next session's bytes. Recovery barrier is `NUL` then `\n` (rejects the joined garbage line, `tools/host_transport.py:264-290`). A standalone NUL line logs only `DIAG_COMMAND_INPUT` (`nos_main.c:872`).
- Rejection output (`nos_main.c:877`): `ERR command input reason=<1 control|2 too_long> bytes= bad= bad_offset= nul_count= first_nul= last_nul=` then bare `DONE` (never an id-tagged DONE; there is no `BEGIN`).
- Where it runs: `cmd` task, per byte read with `usb_serial_jtag_read_bytes(&ch,1,50 ms)` (`nos_main.c:889`).
- Files: `nos/command_input.c:7-27`, `nos/command_input.h`, `nos_main.c:882-922`, `tests/command_input_test.c`.

### Request envelope (nos/host_request.c, nos/host_request.h)
- `host_request_parse` (`host_request.c:3-17`): a line starting `req` followed by NUL/space/tab is an envelope candidate; otherwise legacy (`0`). Candidate is valid only if `req <8 lowercase hex> <command>` with exactly one space each, command non-empty and not starting with a space, total length 14..191, no control bytes. Anything else returns -1 and the task prints `ERR command envelope invalid` + bare `DONE` (`nos_main.c:900`). Bare `req` is therefore "invalid", while `request foo` is legacy and falls to `ERR unknown`.
- Effective command length inside an envelope is 191-13 = 178 bytes (`hostcaps` advertises `max_command=191`, which is the whole wire line; `tools/host_transport.py:190` validates the wire string against it).
- Every command text may be enveloped, including all transfer verbs; the envelope adds `BEGIN <id>` before output and `DONE <id> ok=0|1 [code=command|busy|transfer]` after (`nos_main.c:855-865, 1794-1804`). Raw `sdput` payload bytes and `SGA`/`SGR` acks bypass framing and envelope entirely (consumed by `io->read` in `sd_xfer.c`).
- `host_request_transfer` (`host_request.c:18-25`) matches the leading token exactly against 8 verbs (`sdfeatures, sdls, sdget, sdget2, sdput, sdput!, sdcrc, sdrm`).
- Legacy (un-enveloped) UI commands get a bare `DONE` (`host_done`, `nos_main.c:855-859`); legacy transfer verbs get NO terminator.
- Exceptions to DONE: `usb_diag`/`usb_guard` without an id emit only their checked `USBDIAG` frame (`nos_main.c:1801`).
- Files: `nos/host_request.c`, `nos/host_request.h`, `tests/host_request_test.c`, `tests/nos_host_protocol_test.c`.

### Command task (task-cmd) and queue
- Purpose: parse USB bytes, run SD transfers, poll BLE transfer queue, hand UI commands over.
- Runs: `xTaskCreate(boot_worker_task,"cmd",6144,...,4,...)` (`nos_main.c:1891`), blocked on a notification until `xTaskNotifyGive(s_command_handle)` at `nos_main.c:1956`, i.e. after board/OS init; bytes sent before that sit in the 4096 B driver RX ring (`config.rx_buffer_size=4096`, `nos_main.c:1883`). Loop period = 50 ms idle (read timeout) or per byte; `ble_xfer_poll()` once per iteration (`nos_main.c:888`).
- Dispatch rules (`nos_main.c:893-919`):
  - transfer verb: `xSemaphoreTake(s_host_reply_lock, 3000 ms)`; on failure `[BEGIN id] ERR command busy` + `DONE [id ok=0 code=busy]`. Holds the lock for the whole transfer, so the UI loop (which only `Take(...,0)`) cannot run a command meanwhile.
  - other: `cmd_line{text[192],request_id[9]}` into `s_cmd_q` with 50 ms wait; full queue gives `[BEGIN id] ERR command queue full` + `DONE ok=0 code=busy`.
- Diagnostics counters `nos_usb_diag_usb_line/queue_accepted/queue_failed` are bumped here (`nos_main.c:892, 913-915`).
- Reads: USB RX ring, `s_cmd_q`/BLE `s_cmd_q`. Writes: stdout (USB VFS, shared with ESP_LOG so log lines can interleave between reply lines).

### UI-loop dispatcher and cmd_exec (firmware/main/nos_main.c)
- `app_main` loop (`nos_main.c:1977-2290`): `activity=ota_activity_begin(); mutate=activity && s_shutdown_phase!=3` (`1984`). One queued command is picked up per iteration (`command_pending`, `1992-1994`) if `s_host_reply_lock` is free. Latency: one frame (ON), ~20 ms (OFF/AOD loop with `vTaskDelay(20)` at `2103/2127`), so a command works with screen off.
- "Quiet" prefix/equality list (`1997-2002`) suppresses the wake: `diag log, prefs*, sports, mem, history_virtual*, settings_carry*, frame_priority*, wake*, imu, off, knock*, walk*, steps, info, state, nav_state, scroll_state, mblur_bench, usb_diag*, usb_guard*, asset_state, card_state, orrery state, orrery basis *, touch timeout *, card_*_check, card_work, bench end, card_profile *, hostcaps, runtime_state, crash*, board retry, exposure_worker*, native_stage*, fps_cap*, list_profile*, list_samples*, touch_state, board, jobs, te`. Any other command calls `go_state(PWR_ON)` first when `mutate` (`2003`), including pure status ones such as `bat`, `sd`, `network status`, `chat`, `ext`, `help`.
- Read-only allowlist `host_command_readonly` (`1805-1813`) is what still runs when `mutate==0` (OTA flashing, or an SD mutation holding owner 2, or shutdown phase 3): `hostcaps runtime_state "diag log" state nav_state scroll_state touch_state board jobs te info mem prefs sports imu wake steps knock bat asset_state card_state "orrery state" mblur_bench crash ext "ext help" "ota cancel" "power restart"`, plus prefixes `crash read ` and `usb_diag `. Everything else returns `ERR update activity busy`, `DONE ok=0 code=busy` (`1814-1820`). `usb_guard`, `prefs flush`, `sd`, `network status` are not on it.
- `cmd_exec` (`1180-1792`): logs `DIAG_COMMAND` with a hash of the verb only (`1183-1187`, so credentials are not logged), sets `s_cmd_ok=1`; the file-local `#define printf cmd_printf` (`1179`) clears it whenever a line starting `ERR` is printed, and handlers can return 0. `DONE ok=` therefore means "accepted, no ERR printed", not that an asynchronous effect finished (`tap`, `swipe`, `btn`, `gb`, `ext rec`, `lte ...`, `board retry`, `sd mount` return ok=1 on scheduling).
- Several diagnostics run synchronously on the UI task and stall frames: `card_qz_check` (65536 cases), `card_light_check`, `card_shade_check`, `card_geometry_check`, `orrery basis check|dates|FIRST COUNT`, `mblur_bench`, `rotation_bench`, `cost_calibrate`.

### SD transfer engine (firmware/main/sd_xfer.c)
- Purpose: one command set for USB and BLE (`xfer_io{read,line,block}`; USB `{usb_read,usb_line,2048}` at `sd_xfer.c:616`; BLE `{ble_read,ble_line,4096}` at `ble_xfer.c:104`; the envelope-aware USB variant `s_host_transfer_io` at `nos_main.c:854` also 2048). Called from `cmd` task only (USB and BLE both), so USB and BLE transfers never overlap; `transfer_claim` (`sd_xfer.c:133`) also guards mount-time recovery (`sd_xfer_recover`, called from `board.c:608`).
- Verb parse (`sd_xfer_exec`, `532-604`): `sdfeatures` replies `SDXFER v=2 paced_get=1 typed_errors=1 safe_put=1 restore_put=1` (`534`). Paths via `sscanf("%127s")` so a path is silently cut at 127 chars and cannot contain whitespace. If no verb matches or arguments are missing the function returns 0 without printing (`544`).
- Mutating verbs (`sdput`, `sdput!`, `sdrm`) first take the OTA scope `ota_external_activity_begin` (`547`, `ota.c:93-107`: refuses while OTA flashing `busy==1`, waits up to 200 ms for the UI to release owner 2); the scope is held for the whole transfer and released with `ota_external_activity_end` (clears cached runtime-admission, `ota.c:109-118`). While held, the UI loop's `ota_activity_begin()` fails (`ota.c:75-87`) so `mutate==0`: touch, button, wake and prefs persistence are suspended and non-allowlisted USB commands answer busy.
- Every verb starts with `recover_transaction` (`557`); if it cannot complete, only `sdls`, and `sdget/sdget2/sdcrc` of an existing file, still work (`562`); everything else returns `<PREFIX> ERR recovery <errno>`.
- Path guard `full_path` (`81-95`): strips leading `/`, rejects substring `..`, `\`, `:`, empty components (`a//b`, lone `.`); root `/sdcard`; total <160.
- Reserved names (refused by `sdput`/`sdput!`, `283-290`): `.sdxfer.txn`, `*.sdxfer-old`, `*.part` (case-insensitive). Not checked by `sdrm`, `sdget`, `sdcrc`, `sdls`.
- Protected names (`291-297`): `watch/settings.0.bin`, `watch/settings.1.bin`, `watch/wifi/settings.0.bin`, `watch/wifi/settings.1.bin`, `imu/intent.0.bin`, `imu/intent.1.bin`. `sdrm` refuses them (`594`, `SDRM ERR protected 1`); plain `sdput` refuses them (`SDPUT ERR restore_required 1`, `351`); `sdput!` may replace them; reads are unrestricted. `watch/wifi/*` can never be restored: `restore_claim` returns -1 and `do_put` answers `owner_busy` (`352-355`, `317`). For `watch/*` the restore also needs `prefs_pending()==0` unless `prefs_loading()`.
- `live_path` (`298-310`): refuses `sdput`/`sdrm` of anything under `imu/` while `imu_worker_pending()`, and of the active recording CSV (`imu/<record.name>`), reply `ERR active 16`.
- `sdput <path> <size>` / `sdput! ...` (`do_put`, `343-398`): checks (reserved, live, protected, `owner_busy`, existing `.sdxfer-old` or `.part` -> `ERR staged 17`) -> `make_parents` -> `create_file(<path>.part, O_EXCL)` -> `SDPUT READY` -> blocks of 2048 (USB) / 4096 (BLE) bytes, each read with a 3 s stall timeout (`SDPUT ERR timeout 110 offset N`), written, CRC32 (`esp_rom_crc32_le`), acked `SDK <bytes>` (host sends the next block only after the ack) -> `durable_close` (fflush+fsync+fclose) -> re-read verify of size/CRC (`SDPUT ERR verify`) -> for protected files `validate_restore` (`prefs_disk_validate_file`, or the `IMU2` header/FNV check for `imu/intent.*`, `SDPUT ERR structure 22`) -> write journal `/sdcard/.sdxfer.txn` (144 B struct: magic `0x53445831`, size, crc, rel[128], FNV check; `178-190`) durably -> `recover_transaction`: re-verify, claim restore owner (prefs or IMU), rename live->`.sdxfer-old`, `.part`->live, verify, delete backup and journal -> `SDPUT OK <size> <crc32hex>`. Size 0 is legal; no free-space precheck. A failed or timed-out upload retains `<path>.part` (`384`), so the next `sdput` to that name fails with `staged 17` until `sdrm <path>.part`.
- `sdget` (`do_get`, `400-425`): whole-file CRC pass, then `SDGET <size> <crc>`, `SDG <base64>` lines of 1536 raw bytes, `SDGET END`; unpaced (host cannot slow it). `sdget2` (`427-530`): 1104-byte chunks `SDG2 <offset> <chunk-crc> <b64>`, stable declared length (appending files are cut at open size, final `file_changed` check), one frame in flight, host `SGA <next>` acks or `SGR <offset>` retries, ack wait 3 s, 5 retries (about 18 s), stale/duplicate acks ignored, errors `SDGET2 ERR <ack_offset|ack_timeout|file_changed|read_crc|...> errno`.
- `sdcrc` -> `SDCRC <size> <crc>` / `SDCRC MISSING`. `sdls [dir]` -> `SDLS <size> <name>[/]`... `SDLS END` (each line capped at 191 chars by `reply`'s 192 B buffer, names with spaces cannot be re-fetched). `sdrm` -> `SDRM OK` or `SDRM ERR protected|active|remove`.
- Error format: `<PREFIX> ERR <tag> <errno>`; envelope sets `ok=0 code=transfer` when any reply contains ` ERR ` (`nos_main.c:851`).
- Files: `firmware/main/sd_xfer.c`, `sd_xfer.h`, `tests/critical_transfer_test.c`, `tests/storage_xfer_fault_test.c`.

### BLE transfer service (firmware/main/ble_xfer.c)
- Purpose: same `sd_xfer_exec` over GATT service `7f5e0001-5a1d-4c3b-9e0f-6e4f53584652` ("NOS XFR"); CTRL `...0002` (write + notify) carries command lines and `\n`-terminated reply lines; DATA `...0003` (write-no-response) carries raw `sdput` bytes into an 8192 B stream buffer.
- Registered next to the Watch and NUS services (`hal_esp32.c:1293-1296`). Advertising is general-discoverable, undirected, name `Bangle.js NOS` (`hal_esp32.c:1313-1328`), whenever BLE is enabled; `ble_hs_cfg.sm_bonding=0`, `sm_sc=0`, `io_cap=NO_INPUT_OUTPUT` (`1558-1560`); characteristic flags have no encrypt/authenticate bits (`ble_xfer.c:73-77`). One connection total.
- Command intake (`ctrl_access`, `51-65`): write of more than 159 bytes fails with `INVALID_ATTR_VALUE_LEN`; CR/LF stripped; empty ignored; `xQueueSend(s_cmd_q,line,0)` into a depth-2 queue of 160 B items, silently dropped if full; `s_conn` updated by whichever central wrote last. Queue and stream buffer are created lazily on the first write (`ensure`, `38-46`) and never freed.
- Execution (`ble_xfer_poll`, `114-131`): called from `cmd_task` once per loop; pops one line, requests a fast connection interval (6-12 units, latency 0) via `ble_gap_update_params`, resets the stream buffer, runs `sd_xfer_exec(line,&BLE_IO)`; unrecognised or malformed lines answer `SDXFER ERR unknown`; 5 s after the last command the interval returns to 64-96 (80-120 ms), latency 3. Replies are chunked to MTU-3 (<=256) and retried up to 200 ticks on `BLE_HS_ENOMEM` (`ble_line`, `86-102`).
- Verbs available over BLE: only the 8 SD verbs. Not available: `req` envelope (it would be `SDXFER ERR unknown`), `hostcaps`, every UI command, `gb`. `tools/sdsync.py --ble` therefore skips `hostcaps` negotiation (`Card(link, negotiate=not o.ble)`, `sdsync.py:370`).
- Runs: serial command task, at most every 50 ms; works with the screen off. Does not take `s_host_reply_lock`, does not use `nos_usb_diag_reply_begin`.
- Files: `firmware/main/ble_xfer.c:14-132`, `hal_esp32.c:1289-1296, 1313-1328, 1420, 1558-1560`.

### USB diagnostics and guard (firmware/main/nos_usb_diag.c, nos_usb_diag.h)
- Purpose: instrumentation of USB-Serial-JTAG writes and an optional override of `usb_serial_jtag_is_connected()` during replies, for benchmark runs (`tools/benchmark_sharp.py:50-66`).
- Linker wraps `usb_serial_jtag_is_connected` and `usb_serial_jtag_write_bytes` (`firmware/main/CMakeLists.txt:91-92`). `__wrap_..._is_connected` (`nos_usb_diag.c:41-52`) counts checks/false results and, only when `guard_enabled` and the caller is the registered reply task, returns true instead of false (`connected_overrides++`). `__wrap_..._write_bytes` (`53-64`) accumulates call counts, failures, total/max microseconds for zero-wait and waited writes.
- Commands (`nos_usb_diag_command`, `70-100`): `usb_diag <nonce>` and `usb_guard <0|1> <nonce>`; nonce is a decimal uint32. Malformed arguments still produce a frame with `nonce=0 valid=0` and never change the guard. Reply is one direct, non-VFS frame `USBDIAG <len>:v=2 nonce= valid= guard= overrides= cc= cf= zc= zf= zu= zm= wc= wf= wu= wm= rx= qa= qf= ui= rc= ri= rq= ra= crc=<crc32hex>` written with `usb_serial_jtag_write_bytes(...,50 ms)`.
- Gating/lifecycle: counters start only after init (`nos_usb_diag_start`, `nos_main.c:1957`), `guard_enabled` is RAM-only, default 0, reset on reboot; `usb_diag` is on the OTA-time allowlist, `usb_guard` is not.
- Reply task registration (`nos_usb_diag_reply_begin/end`) brackets each UI command (`nos_main.c:1796/1803`) and each USB transfer (`nos_main.c:903/905`).

### Extended-strap debug commands (firmware/main/ext/ext_debug_commands.inc, ext_sys.c)
- Shared by device and simulator (`sim/nos_ext_sim.c:857` includes the same `.inc`). Entry: `hal_ext_debug_command` (`ext_sys.c:225-253`) via `cmd_exec` prefix `ext` (`nos_main.c:1596`); `ext` or `ext status` prints status (`EXT board_ok= present= loops= ...`, `scheduling`, stack, button, per-module debug); `ext help` prints the usage line.
- Verbs (`ext_debug_dispatch`, `19-57`): `recstop`, `gps off`, `lte off`, `hr off`, `gest off` (always allowed, even during shutdown); then, unless `ext_shutdown_requested()`: `haptic 0..6` (needs `EXT_HAPTIC`), `sfx 0..13`, `vol 0..100` (need `EXT_SPEAKER`; B has no built-in audio), `play <path>`, `rec <path>` (`hal_audio_play_file`, `hal_rec_start_exclusive`; enqueue, completion async, need audio strap/mic and `nos_runtime_recording_allowed()`; path is used as given, only length-checked, so none of the `sd_xfer` path/protected/reserved guards apply), `gps on`, `lte on`, `lte at <cmd>` (raw AT to the modem, `ext_lte.c:564-576`), `lte sms <num> <text>` (sends a real SMS, `577-583`), `hr on` (`EXT_HR`), `gest on` (`EXT_LIGHT`), `mag cal|done|map <+x+y+z> [<fwd>]` (`map` writes NVS `ext_mag` via `hal_store_save_checked`, `ext_mag.c:294`), `sea <800..1200 hPa>` (`EXT_ENV`; `hal_env_set_sea_level` writes NVS `ext_sea`, `ext_env.c:192-197`). Return 0 for unknown or unsupported (DONE ok=0).
- The comment says positive result "acknowledges a bounded request, not asynchronous completion".

### Gadgetbridge line injection (`gb ` in cmd_exec, nos/svc.c)
- `gb <line>` at `nos_main.c:1594` calls `svc_phone_inject(s+3)` (`nos/svc.c:1202-1213`): copies the line into `B->line` and runs `handle_line`, the same path as a line from the phone's NUS (`phone_tick`, `svc.c:1236-1241`, up to 2048 B). Over USB the cap is 188 bytes (191 minus `gb `).
- Accepted forms (`nos/gb.c:96-149`): `GB({...})` (also bare `{"t":...}` JSON), `setTime(<epoch>)`, `setTimeZone(<hours>)`. Effects (`svc.c:1148-1191, 1000-1019`): `notify/notify~/notify-`, `call`, `musicinfo/musicstate`, `weather`, `find`, `alarm` (replaces ALL alarms from the array and calls `svc_alarm_save`), `act` (starts realtime HR/step push to the phone), `vibrate`, `is_gps_active`; `setTime`/`setTimeZone` change `nos_prefs.tz_min` (+`prefs_save`) and write the RTC through `hal_set_datetime`. Replies (`txf`) go to the phone link, not to USB, so `DONE ok=1` is returned even if nothing was parsed.
- Runs in the UI task (queue drained in the loop), so it shares `B->line` safely with the tick. Not quiet: it wakes the screen.

### Host tools
- `tools/host_transport.py`: shared framing used by `watchctl.py`, `sdsync.py`, `flash_watch.py`, `voice_setup.py`. `validate_command` (1..191 UTF-8 bytes, no leading space, no control bytes), `HostTransport.negotiate` (sends `hostcaps`; absence of any reply fails closed, only `ERR unknown:`+DONE selects legacy), per-command random id (`secrets.token_hex(4)`), reads `BEGIN`/`DONE <id> ok=` correlation, `resynchronize` (NUL+LF then an enveloped `hostcaps` barrier that must return `NOSHOST` plus matching `DONE`), `CommandTimeout.ambiguous` for non-read-only commands (never replays mutations; retries only `is_read_only`: `help hostcaps board info time prefs apps sd sdfeatures imu chat "chat status" "chat history" te` + `sdcrc|sdget|sdget2|sdls`). `redact()` masks `chat key|wifi`, `wifi`, `password=`.
- `tools/watchctl.py`: CLI/script runner; `--smoke` uses `runtime_state` (`NOSRUNTIME` idle gating), `info`, `open`; `--sync-time` and `--wifi-ssid` send legacy WOS commands (`tz`, `settime`, `time`, `wifi`) and are refused when `info` identifies NOS (`watchctl.py:288-293`).
- `tools/sdsync.py`: USB (finds serial `28:84:85:B0:8B:30`, else the only Espressif VID 0x303A port; DTR/RTS low so opening does not reset the chip) or BLE (`bleak`, name `Bangle.js NOS`, MTU-3 writes without response); modes mirror (default `build/sd` -> card root, CRC-skip unchanged), `--ls`, `--get`, `--pull` (recursive, skips `System Volume`), `--rm`, `--restore-protected` (uses `sdput!`, requires `restore_put=1`). Uses `sdget2` when `paced_get=1`, retries a failed GET up to 3 times with NUL resync (USB only), never replays writes. `safe_path` refuses whitespace, `\`, `:`, `.`/`..` segments.
- `tools/flash_watch.py`: checked flasher. Verifies the build bundle and flash ranges (`build_identity.verify`, `flash_layout.checked_layout`: ranges must not overlap NVS `0x9000..0xF000`), then sends `prefs flush` through `HostTransport` and requires exactly one `NOSPREFS` with `ok=1 stored=1 match=1` (`flash_watch.py:454-467`), re-verifies the artifacts, then `esptool write_flash` at 460800 baud with `default_reset` / `hard_reset` (`580-582`). `--boot-recovery` and `--legacy-c-backup` are ROM-mode paths with evidence receipts. It never runs `board` or compares the device's `board` string to the build, so port/board confirmation stays manual (see Gaps).
- `tools/diag_export.py`: offline decoder of files copied from `/sdcard/watch/diagnostics/diag-00..07.bin` (`nos/diag_log.c:24,395`); 32 B file headers and 44 B records `<IHHIIIIHHiiiI>`, CRC32 checked, replayed (boot,seq) deduplicated, codes include `COMMAND 0x202`, `COMMAND_INPUT 0x203`, `COMMAND_INPUT_DETAIL 0x204`; `--json/--csv/--strict` (exit 2 on corruption). Never touches a serial port.

## USB command map

Handler locations are `firmware/main/nos_main.c` unless stated. Class: **RO** read-only status, **MUT** state-mutating (RAM/run-time only), **DBG** debug or bench, **DEST** destructive, **PERS** persistent. "Capture" = refuses with `ERR <verb> capture_active` while `bench begin` is live; "touch" = refuses with `touch_active` while a touch/injection is active.

### Identity, status and diagnostics (all RO unless noted)

| Command | Handler | Effect / output | Class | Gating |
|---|---|---|---|---|
| `hostcaps` | 1188 | `NOSHOST v=1 req=1 max_command=191 typed_errors=1` | RO | on allowlist; quiet |
| `runtime_state` | 1189, 1008 | `NOSRUNTIME v=1 ota_busy= ... radio_pending=` cached, lease-free | RO | allowlist; quiet |
| `touch_state` | 1190, 607 | `NOSTOUCHSTATE` | RO | allowlist; quiet |
| `board` | 1191, `board.c:1077` | `BOARD v=2 <name> flash psram touch imu rtc ... heap`, `BOARD_DEVICE`, `BOARD_WORKERS` | RO | allowlist; quiet; shows B/C identity |
| `te` | 1192, 999 | `NOSTE` TE sync stats | RO | allowlist; quiet |
| `jobs` | 1193 | `JOB id= state= age_ms= ...` per background job | RO | quiet |
| `diag log` | 1204, `nos/diag_log.c:328` | `NOSLOG`, `NOSEVIDENCE`, `NOSFAULT` | RO | allowlist; quiet |
| `crash` | 1150, `nos_heap_diag.c:404` | `NOSCRASH` + `NOSCRASHBT` of the coredump image | RO | allowlist (exact) |
| `crash read OFF N` | 1157-1178, `nos_heap_diag.c:380` | `NOSCRASHDATA offset= bytes= hex=` (N 1..256 per call, raw partition read) | RO | allowlist prefix |
| `usb_diag NONCE` | 1214, `nos_usb_diag.c:70` | `USBDIAG` checked frame | RO | allowlist prefix; quiet |
| `state` | 1670 | `NOSDISPLAY`, `NOSRENDER`, `NOSSTATE ...`, `NOSSHUTTER`, `NOSPACING`, `NOSFPSCAP`, list/native/sched lines | RO | allowlist; quiet |
| `nav_state`, `scroll_state` | 1661, 1656 | `NOSNAV`, `NOSSCROLL` | RO | allowlist; quiet |
| `asset_state`, `card_state`, `orrery state`, `card_work`, `card_profile report` | 1696, 1704, 1564, 1508, 1526 | asset worker, Orrery card state/cost reports | RO | asset/card/orrery on allowlist; quiet |
| `info` | 1705-1779 | huge dump: panel, TE, radios, heap, `NOS <ver> board=`, BLUR/PBLUR/MBLUR, `NOSBUILD` (source SHA256, ELF SHA256, IDF, QSPI Hz), power, stack high-water marks (`NOSUISTACK/NOSCMDSTACK/NOSTOUCH`), `NOSCACHE` | RO | allowlist; quiet |
| `mem` | 1642 | PSRAM allocation table, `MEM heap_used/psram_free` | RO | allowlist; quiet |
| `sd`, `sd mount` | 1780 | `SD mounted= total= free= status=`, `FLASH ...`; `sd mount` also `hal_storage_retry()` (queues a remount, board worker reports later) | RO / MUT | `sd` not on allowlist, wakes display |
| `wake` | 1597 | `WAKE on= wrist= pickup= turn= learned= ... bias=` | RO | allowlist; quiet |
| `bat` | 1608 | `BAT rail= cal= percent= valid= external=` | RO | allowlist; wakes display |
| `imu` | 1613 | `IMU v= a=(..) g=(..)` one sample | RO | allowlist; quiet |
| `steps`, `knock` | 1619, 1620 | pedometer day total / knock detector count | RO | allowlist; quiet |
| `prefs` | 1621 | `NOSPREFS ok= stored= match= hash= stored_hash= ...` RAM vs NVS "nosprefs" blob (not the SD copy) | RO | allowlist (exact); quiet |
| `sports` | 1637 | `NOSSPORTS mode= queued= written= dropped= errors= available= cpu_max=` | RO | allowlist; quiet |
| `fps_cap`, `list_profile`, `list_samples I N` | 1265, 1252, 1263 | report cap / profiler status / up to 8 `NOSLISTFRAME` rows | RO | quiet; samples refuse during capture |
| `network status` | 1537 | `NETWORK wifi=.. ble=..` | RO | wakes display |
| `network scan` | 1550 | cached Wi-Fi APs and BLE devices (`hal_wifi_scan` starts an async scan, `hal_ble_scan` reads cache) | RO (starts RF activity) | wakes display |
| `network setup` | 1541 | prints the temporary Phone-setup SSID and password (`SETUP active=1 ssid= password=`) | RO (secret) | wakes display |
| `help` | 1788 | usage line; incomplete (omits `hostcaps runtime_state touch_state board te jobs crash power ota network chat prefs sports bat batcal steps knock face fps_cap list_* frame_pacing ...`) | RO | - |
| any other | 1790 | `ERR unknown: <raw text>` (echoes the line verbatim) | - | - |

### Bench and debug (DBG; RAM only, lost on reboot)

| Command | Line | Effect |
|---|---|---|
| `bench begin` / `bench end` | 1223 / 1236 | arm/stop frame capture (`NOSCAPTURE`, `NOSBLURCAP`, `NOSCACHECAP`, cost, pipeline, list, `NOSDAMAGE` reports); begin refuses if capture live or `trace>0`; `bench end` is a read-only-ish report and quiet |
| `trace N` | 1587 | print next N painted frames (`NOSF`); N>0 aborts a live capture |
| `perf` | 1562 | toggle 1 Hz `NOSPERF` line |
| `pipeline_probe 0|1`, `cost_probe 0|1`, `cost_calibrate`, `rotation_bench`, `mblur_bench`, `list_profile 0|1` | 1293, 1282, 1276, 1279, 1652, 1254 | pipeline/cost instrumentation; `list_profile 1` allocates a PSRAM profiler (`ERR list_profile no_memory` on failure); all capture-gated, `rotation_bench` also touch-gated |
| `frame_pacing 0|1`, `frame_priority 0|3`, `fps_cap` read | 1286, 1307 | pacer mode; UI task priority policy |
| `settings_shutter`, `settings_carry`, `history_virtual`, `history_native_copy`, `native_rom`, `damage_bands`, `rotation 0..2`, `cache_packed`, `prog_geometry`, `history_cadence`, `history_uniform`, `history_packed`, `exposure_worker`, `native_stage 0..3`, `layers`, `history`, `full` | 1297-1345, 1349, 1392-1454, 1527, 1532 | render/cache feature switches; most are capture-gated, the `settings_carry/history_virtual/history_native_copy/native_rom/exposure_worker/native_stage/frame_priority` set also touch-gated; `layers/history_uniform/history_packed/history/full` have no gate |
| `card_qz`, `card_light`, `card_shade`, `card_sun`, `card_pipeline`, `card_detail 2|4`, `card_proxy`, `card_budget 250..8000`, `card_profile 0|1`, `card_*_check`, `orrery basis ...`, `orrery prof N` | 1456-1526, 1522, 1564-1586 | Orrery/solar card optimisation switches and self-checks (`CARD_*_CHECK mismatches=`) |
| `swipe_cold X0 Y0 X1 Y1 MS` | 1353 | release launcher/settings caches then schedule a swipe; only on LAUNCHER/SETTINGS, capture/touch-gated |
| `predict 0|1` | 1563 | touch prediction A/B |
| `touch timeout 0..8` | 1582 | `board_touch_inject_timeouts`: fault injection into the touch driver |
| `wake trace S`, `knock log 0|1`, `walk log 0|1` | 1602, 1617, 1618 | stream detector lines for S seconds / toggle detector logging |
| `board retry` | 1205, `board.c:180` | queue I2C device re-probe (and restart worker tasks if creation failed) |
| `awake` | 1591 | toggle "never sleep" (`s_no_sleep`) |

### UI injection and navigation (MUT, no persistence)

| Command | Line | Effect |
|---|---|---|
| `tap X Y`, `swipe X0 Y0 X1 Y1 [MS]`, `btn` | 1380, 1383, 1387 | synthetic touch (90 ms tap / 220 ms default swipe) in UI coordinates, or a button pulse; `nos_keep_awake`; swallowed when `mutate==0` |
| `open NAME` | 1557, `screen_by_name` 924-937 | `go_state(PWR_ON)` then `nos_open(screen, 233,233)`; prints `OPEN <screen name>` or `ERR no screen <text>`. Names are case-insensitive launcher titles from `nos/apps/registry.c:8-36` (`Activity Sports Energy Jobs Chat "Battery history" "Sensor logs" "Wrist tuning" "Quick report" "Heart rate" Breathe Weather Music Timer Stopwatch Alarms "World clock" Compass IMU Barometer Navigate Cellular Recorder "Find phone" Torch "Developer tools" Diagnostics Settings`; spaces allowed because the whole remainder is the name) plus aliases `control notifs launcher faces power modules about activity heart imu` (`928-932`). Call, alarm-ring, timer-ring, find-watch, battery and connectivity screens are not addressable here |
| `home` | 1561 | `nos_home()`; wakes display |
| `network wifi|ble` | 1533 | open the connectivity screen (`set_connectivity_open`) |
| `aod 0|1` | 1593 | `go_state(PWR_AOD|PWR_ON)` |
| `off` | 1616 | `go_state(PWR_OFF)`: display off only, not power off; quiet |
| `face N` | 1388 | sets `nos_prefs.face` and rebuilds home; no `prefs_save` so it persists only with a later save (`prefs flush`, `bri`, another setting) |

### Persistent (PERS)

| Command | Line | Store written |
|---|---|---|
| `te 0|1` | 1216 | `nos_prefs.display_te` + `prefs_save()` (SD `settings.N.bin` + NVS mirror); `te 1` needs `nos_display_te_snapshot().ready` (TE is not routed on the B, so expect `ERR te unavailable`); capture-gated |
| `fps_cap 24|30|40|48|60|0` | 1268 | `nos_prefs.fps_cap` + `prefs_save()` |
| `bri N` | 1592 | `nos_prefs.brightness=(uint8_t)N` + `prefs_save()`; no range check (256 wraps) |
| `prefs flush` | 1621-1636 | `prefs_save(); prefs_flush()` (synchronous durable write; refused with `ok=0` while `voice_chat_active()`), then compares RAM with the NVS blob; used by `flash_watch.py` |
| `batcal N` | 1607, `hal_esp32.c:311` | NVS key `bat_cal` (clamped +-400 mV), immediate |
| `gb {"t":"alarm",...}` / `gb setTime(..)` / `setTimeZone(..)` | 1594 | alarms via `svc_alarm_save`; time zone in `nos_prefs.tz_min`; RTC time via `hal_set_datetime` |
| `wake learn` | 1603 | `wake_wrist_sync(1)` then `wake_service_learn()`; learned bias stored by `wake_service` (`wake_cfg` through `hal_store_save_checked`, `core/wake_service.c:63`) |
| `ext mag map ...`, `ext sea <hPa>` | `ext_debug_commands.inc` | NVS `ext_mag`, `ext_sea` |
| `chat key|stt|tts|voice <value>`, `chat wifi {json}`, `chat tier N`, `chat volume N`, `chat audio S G`, `chat select|delete N`, `chat clear`, `chat history flush` | 1595, `voice_chat.c:539-611` | NVS `voice_chat` (checked save + readback) and `voice_hist`; `volume` and `wifi` also set `nos_prefs` (`wifi` forces `nos_prefs.wifi=1`, `prefs_save()`, and starts a Wi-Fi join with the given credentials) |
| `sdput`, `sdput!` | `sd_xfer.c:343` | SD files, journal `.sdxfer.txn` (see SD engine); `sdput!` can replace `watch/settings.N.bin` and `imu/intent.N.bin`, which reload RAM owners (`prefs_restore_end`, `imu_checkpoint_restore_end`) |

### Destructive or hardware-affecting (DEST)

| Command | Line | Effect | Gating |
|---|---|---|---|
| `crash clear` | 1151, `nos_heap_diag.c:395` | `esp_core_dump_image_erase()`, erases the stored coredump (then `NOSCRASH cleared=1`) | not on allowlist (blocked during OTA/phase 3); no confirmation |
| `sdrm <path>` | `sd_xfer.c:593-597` | `remove()` of any file or empty dir not protected or live; USB and BLE | protected names refused; reserved names not |
| `sdput`/`sdput!` | `sd_xfer.c:343` | replaces an existing file (backup kept until verified) | as above |
| `power restart` | 1207, 825-837 | `esp_restart()` after `prefs_flush`; only accepted in `s_shutdown_phase==3` ("storage restart required") else `ERR power restart recovery_only`; refuses on OTA/IMU/recording/audio owners | on allowlist so it still runs when `mutate==0` |
| `power cancel` | 1208 | `shutdown_cancel()`: cancels a pending restart/deep-sleep request (can end in phase 3) | wakes display |
| `ota cancel` | 1206, `ota.c:130` | ends the Wi-Fi OTA session token (`ota_session_end`); USB has no OTA upload | on allowlist |
| `chat clear`, `chat delete N`, `chat cancel` | `voice_chat.c` | erase saved chat history / abort a turn | C board only |
| `ext lte sms`, `ext lte at`, `ext rec`, `ext play`, `ext mag cal` | `ext_*` | real SMS, raw modem AT, SD recording file creation (`<path>.part` then link/rename; refuses if the target exists), playback, magnetometer calibration state | needs strap presence/present bits |
| `gb {"t":"alarm"...}` | 1594 | replaces every alarm | - |

### Developer-mode, guard and board gating summary

- **Developer Tools app gating: none.** `nos_dev_overlays` (`nos/developer.c:6`) only draws overlays; no serial verb checks it. The only link is the shared TE preference (`te` command vs the app toggle).
- **usb_guard gating: none for commands.** It only changes what `usb_serial_jtag_is_connected()` returns inside the registered reply task (`nos_usb_diag.c:47`), so replies are not suppressed when the host is not "connected".
- **OTA / shutdown-phase-3 gating:** the read-only allowlist above (`nos_main.c:1805-1820`); also active while an SD `sdput`/`sdrm` or BLE transfer holds owner 2.
- **Run-state gating:** `capture_active` (any bench-capture switch), `touch_active` (`s_inj_active || s_touch_now || s_touch_any`), `trace>0` for `bench begin`, `crash`/`te` hardware availability.
- **C board only:** `chat *` (`voice_chat.c:6` compiles the real handler only under `CONFIG_WATCH_BOARD_143C`; on B every `chat ...` prints `ERR chat requires C board` and returns -1, `voice_chat.c:614`). `tools/voice_setup.py` therefore requires the `board` string to be the C board (`voice_setup.py:93-97`).
- **B-strap only:** `ext` drivers are compiled "absent" when `CONFIG_WATCH_BOARD_143C` (`ext_priv.h:24-29`); on B each subverb additionally checks `hal_ext_present()` bits.
- **Legacy WOS verbs not in NOS (answer `ERR unknown`):** `time settime tz apps wifi wifistop bleinfo` exist only in the dead `firmware/main/main.c:678-884`. `watchctl.py` lists `apps`/`time` in `READONLY`, and `host_transport.is_read_only` lists `apps`/`time`, both stale for NOS.

## How it runs

- **Tasks and priorities:** `cmd` task prio 4, no core affinity, 6144 B (internal RAM) parses USB and runs SD transfers/BLE commands; `touch` prio 5 and `flush` prio 4 are pinned to core 1; the main `app_main` task is core 0, base prio 1, and executes `cmd_exec` (`nos_main.c:1887-1891, 1899`). ESP_LOG shares the USB-Serial-JTAG console (`usb_serial_jtag_vfs_use_driver`, `nos_main.c:1941`) with command replies; one `printf` per line stays atomic, which is why SD downloads are text lines.
- **Boot ordering:** `cmd` is created early (stack reserved before radios) but parked until board/OS init completes (`nos_main.c:1954-1957`); `READY,{...}` and `NOSIDENTITY` are printed immediately after.
- **Screen-off behaviour:** `cmd_task` and SD/BLE transfers are independent of the display state. UI-queue commands run in the ~20 ms OFF/AOD loop; quiet commands do not wake the panel, others do. A transfer that holds the OTA scope makes the loop run read-only: no touch, button or raise-to-wake handling, no `prefs_tick` persistence, until the transfer ends.
- **Back-pressure:** the driver RX ring is 4096 B and drops bytes when full, so uploads are block-acked (2 KB over USB because the ring is 4 KB; BLE writes land in an 8 KB stream buffer, 4 KB blocks). A USB transfer occupies `cmd`, so extra command bytes sent during it are not parsed until it finishes and may be dropped.
- **Command-queue depth:** 1 (`command_pending`) + 4 (`s_cmd_q`) UI commands; BLE queue depth 2.

## Data in / data out

| Direction | Item | Format | Source or sink |
|---|---|---|---|
| in | command line | text, LF-terminated, 1..191 B, optional `req <8hex> ` | host over USB |
| in | transfer payload | raw bytes after `SDPUT READY`, block-acked | host (USB/BLE DATA) |
| in | `SGA <n>` / `SGR <n>` | text lines consumed only during `sdget2` | host |
| in | BLE CTRL write | text command <=159 B (SD verbs only) | any BLE central |
| out | `BEGIN <id>` / `DONE <id> ok=0|1 [code=...]` | text | envelope |
| out | bare `DONE` | text | legacy UI command or input rejection |
| out | status lines | `NOS*`, `BOARD*`, `JOB`, `SD`, `FLASH`, `WAKE`, `BAT`, `IMU`, `EXT ...`, `NOSCHAT*` | cmd_exec |
| out | `SDLS/SDCRC/SDGET*/SDG*/SDPUT*/SDK/SDRM`, `SDXFER` | text | sd_xfer |
| out | `USBDIAG <len>:... crc=` | direct framed text | nos_usb_diag |
| out | `NOSCRASHDATA` hex | text | coredump partition |
| in/out | `diag-NN.bin` | binary, copied via sdsync, decoded offline | diag_export |

## Persistence

- Commands that reach `nos_prefs` (`te`, `fps_cap`, `bri`, `face`, `chat volume|wifi`, `gb` time zone) use the normal debounce: write 2 s after the last change or 10 s after the first, only in a persistence window (`prefs_tick`, `nos_main.c:2098, 2110, 2162`); SD `/sdcard/watch/settings.0|1.bin` authoritative, NVS mirror. `prefs flush` bypasses the debounce.
- Direct NVS keys written by commands: `bat_cal`, `voice_chat`, `voice_hist`, `ext_mag`, `ext_sea`, `wake_cfg`.
- SD: `sdput` leaves staged `<path>.part`, `.sdxfer.txn` and `<path>.sdxfer-old` until publication; boot (`sd_xfer_recover`, `board.c:608`) and every later SD verb complete or reject a ready journal.
- Flash: `crash clear` erases the coredump partition image; `flash_watch.py` writes app/partitions but never the NVS range.
- RAM-only: every DBG toggle, `usb_guard`, injection state, `awake`.

## Interactions

- [[Engine and scheduling]]: `cmd_exec` is called from the `app_main` loop, one command per iteration, ahead of input and the frame; `host_ui_dispatch` and `ota_activity_begin` define `mutate`.
- [[Storage and persistence]]: `sd_xfer.c` is the only remote file path; protected settings files, `.sdxfer.txn` journal, `sd_xfer_recover` at mount, `prefs_restore_begin/end` handshake. (That note lists "`ota` commands via cmd task"; the only USB OTA verb is `ota cancel`.)
- [[Connectivity and voice]]: BLE XFR service, NUS line path equivalent to `gb`, `chat *` verbs (C board), `chat wifi`, OTA session token.
- [[Settings and developer]]: `te`, `fps_cap`, `bri` share preferences with the Settings and Developer Tools screens; Developer Tools does not gate any command.
- [[Hardware and drivers]]: `ext` strap commands, `board`, `board retry`, `touch timeout`, `sd mount`.
- [[Power and battery]]: `power restart|cancel`, `off`, `aod`, `batcal`, `bat`, `awake`.
- [[Services hub]]: `gb` injection into `svc_phone_inject`, alarms/notifications/time effects.
- [[Rendering pipeline]]: bench/debug switches (`bench`, `layers`, `history*`, `native_*`, `card_*`, `rotation`).
- [[Health and activity]] and [[Sports and workouts]]: `steps`, `sports`, `wake`, `imu` status; IMU files are `live_path`-protected from `sdput`/`sdrm`; `imu/intent.*` protected.
- [[Shell and faces]]: `open`, `home`, `face`, `tap`, `swipe`.
- [[Gaps]], [[OS overview]].

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| high | BLE XFR service is unauthenticated, unbonded and discoverable; protected-path list does not stop reads or `sdput!`. Any nearby central can `sdget` `watch/wifi/settings.N.bin` (key `wifi_cred`, stored by `prefs_disk_save` with a checksum, no encryption), `sdls` the card, `sdrm` any non-protected file (diagnostics, workouts, IMU CSVs) and `sdput!` a replacement for `watch/settings.N.bin` (prefs reload) | `ble_xfer.c:73-77`, `hal_esp32.c:1313-1328, 1558-1560`, `sd_xfer.c:291-297, 351, 536, 594`, `hal_esp32.c:243` | Wi-Fi credential disclosure and tampering/deletion of SD data from a phone-range attacker whenever BLE is on and no phone holds the single link |
| medium | Malformed SD verbs on USB are silent: `sdget`, `sdput x abc`, `sdcrc` with no path make `sd_xfer_exec` return 0 and `host_transfer_execute` ignores it; enveloped, the host still gets `DONE <id> ok=1`; un-enveloped nothing is sent. BLE answers `SDXFER ERR unknown` | `sd_xfer.c:544`, `nos_main.c:860-865` | A host that trusts `ok=1` believes a transfer verb succeeded; hangs until its own timeout |
| medium | `flash_watch.py` and `tools/build.ps1 -Flash` never compare the connected device's `board` string or MAC with the build target; the checkpoint accepts any NOS device that answers `prefs flush` with `ok=1 stored=1 match=1`. CLAUDE.md makes port/board verification a manual step (B app at 0x20000, C at 0x10000) | `flash_watch.py:454-467, 539-582`, `tools/build.ps1:52-54` | Wrong COM port flashes the wrong board's image; `hostcaps` carries no board identity |
| medium | UI is frozen during any SD mutation (`sdput`, `sdput!`, `sdrm`, over USB or BLE): the transfer holds OTA owner 2, so `ota_activity_begin` fails and `mutate=0` (no touch/button/wake, no sleep policy, no `prefs_tick`), and non-allowlisted USB commands return `ERR update activity busy` | `sd_xfer.c:547-554, 602`, `ota.c:75-87, 93-118`, `nos_main.c:1984-1990, 1816-1820` | Large or slow (BLE) uploads make the watch ignore touch and not wake for the whole transfer |
| medium | `sdrm` checks only protected/live paths; `.sdxfer.txn`, `*.part`, `*.sdxfer-old` are removable. Deleting the journal orphans a verified staged file; deleting `.sdxfer-old` while the live file is absent destroys the only backup | `sd_xfer.c:283-290 vs 593-597` | Interrupted restore of settings/IMU intent can lose the pre-restore copy; same verb is the only cleanup for failed-upload `.part` files, so it cannot simply be blocked |
| low | Failed or timed-out `sdput` keeps `<path>.part`; the next attempt returns `SDPUT ERR staged 17`; `sdsync.py` has no cleanup | `sd_xfer.c:360-362, 384`, `sdsync.py:172-200` | Retry fails until the user runs `--rm <path>.part` |
| low | `sscanf("%127s")` silently truncates paths >127 chars and the host sees `SDPUT OK`/crc match for the shortened name; tool `safe_path` has no length cap below 191 total | `sd_xfer.c:537-543`, `sdsync.py:298-308` | File lands under a different name than requested |
| low | Names with spaces: firmware `%127s` and tool `safe_path` reject them, but `sdls` reports them; `sdsync --pull` aborts the whole pull with `ValueError` on the first such entry. `SDLS` lines are cut at 191 chars | `sd_xfer.c:75-80, 542`, `sdsync.py:298-300, 322-326` | Cannot fetch or delete such files; pull is all-or-nothing |
| low | Framing has no idle timeout; a partial line persists across tool sessions and corrupts the next command; blank/whitespace lines are silent (no DONE); leading/trailing space is not trimmed; NUL+LF is the only barrier | `command_input.c:7-27`, `nos_main.c:893-896`, `host_transport.py:264-290` | Intermittent `ERR unknown`/command-input rejects after an aborted session; hosts must resynchronize |
| low | Aborted raw uploads fall into the line parser: after a firmware-side `SDPUT ERR ...` any further payload bytes are parsed as commands (a payload containing `\nsdrm x\n` would run). In-tree hosts stop at the error, third-party senders may not | `sd_xfer.c:370-384`, `nos_main.c:889-919` | Only matters for hosts that keep streaming after an error |
| low | `ERR unknown: %s` and `ERR no screen %s` echo the raw line, so a mis-sent credential command (for example the legacy `wifi {...}` form that `watchctl --wifi-ssid` builds) is printed with the password; `network setup` deliberately prints the setup AP password | `nos_main.c:1560, 1790, 1541-1549`, `watchctl.py:300-302` | Secrets can reach a serial log; host-side `redact()` hides them only in the tool's own logs |
| low | Dead verb names in host tooling: `READONLY` in `watchctl.py:~50` and `is_read_only` in `host_transport.py:79` include `apps`/`time`; `watchctl --sync-time/--wifi-ssid` need WOS commands (`tz`, `settime`, `wifi`) that only dead `main.c` implements, so they are refused for NOS | `watchctl.py:50-52, 288-293`, `main.c:678-884` | No supported USB time/Wi-Fi provisioning for NOS except `gb setTime(...)`, `chat wifi`, and on-watch setup |
| low | `help` lists only part of the surface; the two allowlists differ from each other and from "quiet" (`mblur_bench` benchmark is on the OTA-time read-only list; `usb_guard`, `prefs flush`, `sd`, `network status` are not) | `nos_main.c:1788, 1805-1813, 1997-2002` | Operators cannot discover verbs; behaviour during OTA/phase 3 is hard to predict |
| low | Debug/bench input is unvalidated or lightly validated: `bri 300` wraps to 44, `tap/swipe` coordinates are not range-checked (only `swipe_cold` is), `face N` skips `prefs_save` so persistence is incidental; heavy checks (`card_qz_check`, `orrery basis check`) run on the UI task with no capture gate | `nos_main.c:1380-1391, 1456-1467, 1592` | Odd saved brightness, stalled frames during bench commands |
| low | `ext play/rec <path>` bypass the sd_xfer path guard (only `strlen` checked, no `..`/protected/reserved test); `ext lte sms|at` need no confirmation | `ext_debug_commands.inc:41-44`, `ext_audio.c:377-430`, `ext_lte.c:564-585` | USB-only; can write odd paths on the card or send SMS from a script |
| low | Docs drift: `hostcaps max_command=191` means whole wire line (envelope leaves 178); `flash_watch.py` receipt literal cites `nos_main.c:1660` for a past build's assert; Storage note says `ota` commands go through the cmd task (only `ota cancel` exists) | `nos_main.c:1188`, `host_request.c:10`, `flash_watch.py:32`, `docs/os-map/systems/Storage and persistence.md:104` | Mild confusion when reading logs or limits |
