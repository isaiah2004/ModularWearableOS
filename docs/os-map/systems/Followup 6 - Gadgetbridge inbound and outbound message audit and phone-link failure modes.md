---
title: "Gadgetbridge inbound and outbound message audit and phone-link failure modes"
tags: [nos, os-map]
updated: 2026-10-23
---

# Gadgetbridge inbound and outbound message audit and phone-link failure modes

## Summary

The phone link is the Gadgetbridge "Bangle.js" protocol over the Nordic UART Service (NUS). The NimBLE host task (core 0, prio 21) only splits received bytes into lines and queues them in an 8192 B PSRAM ring; everything else (framing, JSON, state, replies) runs in the UI task inside `svc_tick()` (`nos/svc.c:1303`), at most 8 lines per tick. Verified against code on branch NOS-v0.5-Alpha, 2026-10-23. Findings that differ from `docs/PHONE.md` or comments are marked STALE.

Key verified facts:

- 12 inbound JSON message types are handled (`notify`, `notify~`, `notify-`, `call`, `musicinfo`, `musicstate`, `weather`, `find`, `alarm`, `act`, `vibrate`, `is_gps_active`, plus the `setTime(...)` / `E.setTimeZone(...)` statements, which are parsed outside the JSON). Every other type (`calendar`, `nav`, `gps`, `http`, `actfetch`, `intent`, anything new) falls through `gb_message` with no log, no counter and no reply (`nos/svc.c:1148-1191`).
- Watch to phone: 12 line shapes, all built by `txf()` (256 B stack buffer) and sent by `hal_phone_write_line()` (`nos/svc.c:150-159`, `firmware/main/hal_esp32.c:1190-1220`). There is no acknowledgment path in either direction; every user action is applied optimistically or left "pending" (diag result `DIAG_RESULT_PENDING`).
- All loss paths are silent: overlong line (>2047 B), ring full (8192 B), TX mbuf exhaustion, unknown type, token overflow (192) and bad JSON all just discard. The only counters (`s_nus_dropped`, `s_nus_lines`, `hal_esp32.c:1092`) are write-only: nothing reads or prints them.
- No bonding, no encryption, no authentication on NUS RX. Any central that connects and subscribes is treated as "the phone".
- Notifications are RAM-only (one PSRAM block), 20 entries, newest first, oldest silently evicted. They never wake the screen; ring screens (call, find watch) do.
- `core/notif.c` (8-entry legacy store) is compiled into the firmware but has zero callers in built code: a dead duplicate of `nos/svc.c`'s store.

## Components

### svc-nus-transport: NUS GATT service, line assembler and ring (hw-ble-radio, task-nimble-host)

- Purpose: receive phone lines into a ring, notify lines out to the phone. UUIDs: service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, RX `...0002` (WRITE | WRITE_NO_RSP), TX `...0003` (NOTIFY) (`hal_esp32.c:1076-1157`). Registered second in `WATCH_GATT` after the companion watch service and before `BLE_XFER` (`hal_esp32.c:1292-1297`) so companion handles do not move.
- RX path: `nus_rx_access` (`:1133`) copies the mbuf in 128 B slices to `nus_rx_bytes` (`:1117`), which runs in the NimBLE host task. `\n` ends a line; `\r` is skipped; other bytes (including `\x10`, `\x03`, NUL) are appended to `s_nus_asm` (2048 B PSRAM, `NUS_LINE_MAX`). At 2047 bytes `s_nus_asm_drop` is set; the rest of the line is skipped and one `s_nus_dropped++` happens at the `\n`. Zero-length lines are never queued. The callback always returns 0 (no ATT error, so the phone is never told).
- Ring: `s_nus_ring` 8192 B PSRAM, records `[len:2 LE][bytes]`, guarded by `s_nus_mtx` (`nus_push_line`, `:1101`). If `used + n + 2 > 8192` the NEW line is dropped whole (`s_nus_dropped++`), older lines are kept. Capacity is about 20-40 typical notify lines, or 4 lines of maximal 2047 B.
- Pop: `hal_phone_read_line` (`:1163`) uses a zero-timeout `xSemaphoreTake`; if the host holds the mutex it returns 0 and the UI retries next tick. It copies `min(len, n-1)`; with `n = GB_LINE_MAX = 2048` nothing is truncated. The caller then applies `strlen`, so an embedded NUL truncates the line at that point (`svc.c:1239-1240`).
- Reset on link events: `BLE_GAP_EVENT_DISCONNECT` zeroes only the half-assembled line (`s_nus_asm_n/drop`, `:1421`) and clears `g_nus_sub`, `g_nus_mtu=23`. The ring itself is NOT flushed on disconnect, on resubscribe, or on `ble_on_reset` (`:1471`), so queued lines from the previous link are still delivered.
- TX path: `hal_phone_write_line` (`:1190`), UI task only. Appends `HAL_PHONE_EOL "\r\n"` (Gadgetbridge drops the byte before `\n`, see `nos/svc.h:22-27`). Chunk size `g_nus_mtu-3` (20 B if MTU is still 23), capped at 180 B. Each chunk is `ble_gatts_notify_custom(g_med_conn, g_nus_tx_h, om)`. On `BLE_HS_ENOMEM` it retries up to 20 times with `vTaskDelay(1)` (FREERTOS_HZ=1000, so at most 20 ms UI stall per chunk); any other error, or ENOMEM after 20 tries, drops the rest of the line, increments `s_nus_dropped` and, if a fragment already went out, sets `s_nus_tx_resync`. The next line is then prefixed with an extra `\r\n` so the phone's `receivedLine` flushes the fragment. `s_nus_tx_resync` is also set on every NUS subscribe (`:1457`).
- MTU: only learned from `BLE_GAP_EVENT_MTU` (`:1447`); `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256` (`firmware/sdkconfig:764`). Not per connection.
- Connected predicate: `hal_phone_connected()` = BLE enabled AND control clock ready AND `g_nus_sub` (NUS TX CCCD notify on) AND `g_med_conn != NONE` (`:1159`). A bare GAP connection without NUS subscription is "not connected" to services and UI.
- Counters: `s_nus_lines`, `s_nus_dropped` (volatile u32); never read.
- Files: `firmware/main/hal_esp32.c:1070-1220,1292-1297,1447-1458`.

### svc-ble-link: advertising, connection parameters, reconnect, bonding (task-nimble-host)

- Task: `nimble_host`, core 0, prio `configMAX_PRIORITIES-4`, stack 4096 (`hal_esp32.c:1531`), parked until `s_ble_initialized`, then `nimble_port_run()` (`ble_host_task`, `:1502-1510`). Created in `ble_init` (`:1519`), which is driven by `radio_poll()` (`:2365`) called every UI loop iteration from `hal_setup_poll` (`firmware/main/watch_setup.inc:35`, including screen OFF). Admission needs 20480 B internal heap (16384 once the host task exists), 8192 B largest block, 64 KB PSRAM (`radio_admit`, `:91`); on failure it retries every 5000 ms (`radio_fail`, `:80`) and `hal_ble_device_status` shows "Bluetooth deferred / stage N error E".
- Intent: `radio_preferences()` (`nos_main.c:953`) runs each UI loop (and once before `hal_esp32_start_radios` at boot, `:1930`). BLE is requested only when `!prefs_loading() && nos_prefs.ble` (default `nos_prefs.ble=1`, `nos/prefs.c:151`); while the SD settings read is unresolved BLE stays off. `hal_ble_set_enabled` (`:527`) only flips an atomic and sets a flag; the host's callout services it.
- Control timer: `ble_control_tick` (`:1368`) is an NPL callout on the default eventq that re-arms ITSELF every 100 ms (`ble_npl_callout_reset(...,100)`) forever once the stack is synced (first call from `ble_on_sync`, `:1499`). Each tick calls `ble_control` (`:1334`): if disabled it cancels discovery/adv/connect and terminates all handles with `BLE_ERR_REM_USER_CONN_TERM`; if enabled it runs `adv_start()` and `ble_start_scan()`. So advertising is re-asserted within 100 ms of any adv stop, including right after a connection is established (MAX_CONNECTIONS=2 in `firmware/sdkconfig:753`; inferred second-central possibility, not hardware-tested). The callout keeps running even when BLE is logically off (stack retained).
- Advertising: connectable undirected, general discoverable, name `Bangle.js NOS`, scan-response carries the NUS 128-bit UUID so Gadgetbridge's Bangle.js scan filter matches; `BLE_HS_FOREVER` (`adv_start`, `:1312`). Restarted on DISCONNECT, ADV_COMPLETE and failed CONNECT (`:1385,1422,1462`).
- Address: fixed static random `37:59:71:93:B5:D7` set in `ble_on_sync` (`:1485`; shown to users as `D7:B5:93:71:59:37`).
- Security: `ble_hs_cfg.sm_bonding=0`, `sm_sc=0`, `sm_io_cap=NO_INPUT_OUTPUT` (`:1558-1560`), `ble_store_config_init()` RAM store only (`CONFIG_BT_NIMBLE_NVS_PERSIST` unset, `sdkconfig:738`). All characteristics are unencrypted. `BLE_GAP_EVENT_ENC_CHANGE` only logs and records `g_ble_bonded`; `REPEAT_PAIRING` unpairs and retries (`:1433`); `PASSKEY_ACTION` only logs. Therefore the watch stores no bond; reconnect is entirely central-initiated (Gadgetbridge's auto-reconnect by address). The watch never initiates a connection to the phone.
- On connect: logs sec_state, then requests conn params itvl 80-120 ms (64-96 units), latency 3, supervision timeout 8 s (`:1401-1408`). `ble_xfer.c:conn_speed` flips these to a fast set while an SD transfer is active and back to this "gentle" set after 5 s idle (`ble_xfer.c:110-130`). Practical phone-to-watch notify latency is therefore up to about one interval x (latency+1), roughly 0.5 s worst case, plus the up to 1 s service tick when the screen is off.
- On disconnect: `g_med_conn=NONE`, `g_med_have=0`, `g_nus_sub=0`, `g_nus_mtu=23`, `ble_xfer_disconnected()`, assembler reset, `adv_start()` (`:1412-1424`). The handler does not compare the closing handle with `g_med_conn`, so with two connections the first disconnect clears state for both. A second central's CONNECT overwrites `g_med_conn` (`:1392`), redirecting NUS TX notifies to it.
- Single-link variables: `g_med_conn` is both the companion media target and the NUS TX target; `hal_status(HAL_FEAT_BLE)` reports UP for any connection (`:900`), while UI "Connected" strings use `hal_phone_connected() || svc_sys()->phone_connected` (NUS-subscribed only).
- Battery-history side channel: `hal_batt_log_tick` suppresses its periodic dump while `g_nus_sub` is set (`:1281`).
- Files: `firmware/main/hal_esp32.c:490-680,1312-1566`, `firmware/main/nos_main.c:953-961,1930-1932,1987`, `firmware/main/watch_setup.inc:33-36`, `firmware/main/ble_xfer.c`.

### svc-gadgetbridge: framing and JSON (nos/gb.c)

- Purpose: pure functions, no allocation, no statics (caller owns buffers). Runs in the UI task via `handle_line` (`nos/svc.c:1193`).
- `gb_frame_line` (`nos/gb.c:96-149`):
  1. `find_call(line,e,"GB(")` scans the line tracking quotes (`"`, `'`, backtick), `//` and `/* */` comments, bracket depth, and statement boundaries (`;`, `0x10`, `0x03`); a name only matches at depth 0 at a statement start after optional identifier/dot qualifier. Text inside JSON strings, nested calls or comments is never treated as a command. Only the FIRST `GB(` per line is used; a second `GB(...)` on the same line is ignored.
  2. The payload must start with `{` after `GB(` (whitespace allowed). Braces/brackets are depth-counted with `"` and `'` string handling and backslash escapes (backtick not handled inside the payload scan). The closing `)` is not checked. An unbalanced payload yields `json=NULL` and the line is discarded unless it also carries a time statement.
  3. If no `GB(` is found, a bare JSON line is accepted when it begins with `{` after optional whitespace/`0x10`/`0x03` and contains the literal `"t"` (double-quoted); trailing spaces and `;` are trimmed.
  4. `setTime(` and `setTimeZone(` (also `E.setTimeZone(`) are found with the same top-level rule; `call_number` requires a finite number then `)`. Accepted: epoch `0 < v < 2^63`; tz `-14 <= h <= 14`.
- `gb_parse` (`:151-278`): tolerant tokenizer (unquoted keys, single-quoted strings, `undefined`, trailing commas implicitly, `\xNN`/`\uNNNN` decoding at read time). Limits: `GB_TOK_MAX = 192` tokens (`gb.h:39`), nesting depth 24, `GB_LINE_MAX = 2048`. Exceeding any returns -1 and `handle_line` drops the message silently. Trailing garbage after the root object is ignored. `svc_big.json` is one shared 192-token PSRAM struct reused for each message.
- Text: `gb_str` decodes JSON escapes, UTF-8 (invalid bytes taken as Latin-1) and folds to ASCII (`fold`, `:460`); emoji/zero-width/variation selectors dropped, other scripts become `?`, controls and space runs collapse to one space, result trimmed, truncated at the destination size (no ellipsis). Surrogate halves become `?`.
- Accessors: `gb_int` rounds half away from zero and clamps to int; `gb_bool` accepts true/false/null/numbers/"true"/"false"/"1"/"0"; missing numeric key returns the default; numeric strings parse as numbers.
- Files: `nos/gb.c`, `nos/gb.h`, tests `tools/gb_test.c`, `tools/gb_rx_check.py`, `tools/gb_tx.txt`.

### svc-phone-link: line pump, connection state, status (nos/svc.c)

- `phone_tick` (`nos/svc.c:1215-1254`), called from `svc_tick` every frame (30-60 Hz ON; once per second with `dt=1.0` while the screen is OFF via `firmware/main/nos_main.c:2094`; once per second in AOD/SENSOR via `nos_frame_run`, `nos_main.c:2121-2125`). Light sleep is disabled, so the link and these ticks continue with the display off. See [[Engine and scheduling]].
- Connection edge (`s_connected` vs `hal_phone_connected()`): on connect, `s_ver_due = now+500`. On disconnect: `s_act_rt_int=0`, `s_find_phone=0`, any call state forced to 0, Gadgetbridge music cleared (`s_music_src==1`). NOT cleared: `s_find_watch`, notifications, phone weather (`s_wx_gb_ms`), alarms, the line ring.
- Connect handshake: 500 ms after the edge it sends `{"t":"ver","fw":"NOS 0.5.0-alpha","hw":"ESP32-S3 AMOLED 1.43"}` (`FW_VERSION "NOS " NOS_VERSION`, `nos/nos.h:27`; STALE: `docs/PHONE.md` shows "NOS 1.0"), then `send_status()`. Nothing else is sent on connect: no notification resync, no alarm upload, no request for state. Anything that depends on the phone pushing (`musicinfo`, `musicstate`, `weather`, `alarm`) waits for the phone.
- Read loop: up to `LINES_PER_FRAME = 8` lines per tick from `hal_phone_read_line` into `svc_big.line[2048]`, run regardless of connection state (before the `if(!c) return`). Drain rate is therefore ~240-480 lines/s on-screen but only 8 lines/s with the screen off or in AOD.
- `status`: `{"t":"status","bat":%d,"volt":%d.%02d,"chg":%d}` at connect, then when `now - s_status_ms > 600000` (10 min), or battery % moved by 5 or more from the last sent value, or charger state changed (`:1248-1253`). `volt` is `mv/1000` and `mv%1000/10` (2 decimals, `mv<0` clamped to 0). Percent is `hal_power.battery_percent` even when `battery_valid=0` (initial default 50, `hal_esp32.c:287`). `chg = external_power==1`.
- `act` realtime stream: see handler table.
- Files: `nos/svc.c:149-170,1193-1254,1303-1329`.

### svc-notifications: store and handlers (nos/svc.c)

- Store: `svc_big.n[SVC_NOTIF_MAX=20]` (`svc_notif`: id int, app[20], title[40], body[160], t_epoch, read, icon, instance u32) and parallel `phone[20]` flags in one PSRAM block `hal_big_alloc(sizeof(svc_big))` (`svc.c:52-58,1281`). If the allocation fails `B==NULL`, `handle_line` returns immediately and ALL phone traffic is ignored (ring then fills and drops).
- `notif_post` (`svc.c:206`): `notif_find(id)` (linear, by id). If found: remove, keep the old `instance`, re-insert at index 0 with `read=0`. Otherwise new entry at 0 with a fresh non-zero `instance`. If the store holds 20 the oldest (index 19) is overwritten (`s_nn=19` then shift): no DISMISS is sent for an evicted phone notification. `banner` (set for phone and companion posts) calls `nos_banner(app, title-or-body, icon, banner_open)` unless `nos_prefs.dnd`; the banner call also fires `nos_feedback(FB_NOTIFY)` (haptic `HAPTIC_BUZZ` if `nos_prefs.haptics`, `SFX_NOTIFY` if `nos_prefs.sound`) and `nos_keep_awake()` (`nos/nos.c:509-521`). Under DND there is no banner and no buzz; the entry is stored unread.
- `gb_notify` (`:1021`): reads `id` (missing -> 0, so id-less messages overwrite each other at id 0), `src` -> app (empty -> "Phone"), `title`, falling back to `sender`, then `subject` (subject also becomes body when body empty), `body`. `tel`, `sender` (beyond fallback), `category` and everything else are ignored. Icon derived from app string (`icon_for`, `:173`: message apps/mail, calendar, phone/dialer/call, weather, music/spotify, map, else bell).
- `notify~` (`change=1`): looks up the id; if absent or not phone-origin the message is dropped; otherwise overwrites only the `title`/`body` fields present, sets `read=0`, bumps the revision. No re-ordering, no banner, no buzz.
- `notify-`: `notif_remove_at(notif_find(id))`; unknown id is a no-op (`notif_remove_at` rejects index -1). No tombstone is kept.
- Ordering consequences (all by construction, no sequence numbers): `notify~` before its `notify` is lost; `notify-` before its `notify` is lost and the later `notify` creates a notification that is never removed; a repeated `notify` with the same id (GB re-posts on app updates) moves to top, marks unread and buzzes again; two apps using the same id replace each other.
- Watch-to-phone actions: `svc_notif_dismiss(i)` (swipe in list or DISMISS in detail) and `svc_notif_clear()` (CLEAR ALL, one line per phone-origin entry, up to 20 lines in one tick) send `{"t":"notify","id":N,"n":"DISMISS"}` only if `phone[i]` and `hal_phone_connected()`; when disconnected the dismissal is local only and never replayed. `svc_notif_open_on_phone(i)` sends `{"t":"notify","id":N,"n":"OPEN"}`, marks it read; the detail screen then toasts "Opened" without any delivery confirmation (`nos/apps/sys_notifs.c:1037-1040`). Opening the list calls `svc_notif_mark_all_read()`; read state is never sent to the phone.
- Companion path: `companion_tick` (`svc.c:1256`) drains up to 4 `hal_notification_poll` entries per tick into `notif_post(app,title,"",s_local_id--,phone=0,banner=1)`. Source queue is an 8-entry FreeRTOS queue of `{app[20],title[40]}` filled by the TIME_CHR `0xF2` frame (BLE) or HTTP `/notify` (`hal_esp32.c:396-423,847-861,1791`). A full queue rejects the write (`BLE_ATT_ERR_INSUFFICIENT_RES` / HTTP 503). Local ids count down from -1 and are never synced to the phone.
- Persistence: none. Notifications live only in PSRAM and are lost at reboot.
- Legacy duplicate: `core/notif.c` (`MAX_NOTIF 8`, `notif_t{app,title,accent}`) is listed in `firmware/main/CMakeLists.txt:18` but has no callers in `nos/**`, `firmware/main/*.c` except legacy `main.c` (not built) and `sim/main_win32.c`; so there are three notification structures (`core/notif.c` dead, `hal_notification` 8-slot queue, `svc` 20-slot store) of which only the last two are live. `svc_notif_add_local` is used only by the sim and tests.
- UI: [[Shell and faces]] / `nos/apps/sys_notifs.c` (list `scr_notifs`, detail `scr_notif_detail`). The list keys cards by (id, instance) so store mutations during a swipe cancel or retarget correctly (`sync_cards`, `:213`). Detail D.phone gates OPEN: without a link it shows "No phone" and `FB_ERROR`.
- Screen-off behaviour: handled at the 1 Hz service tick. The banner state is set and a buzz/sound fires, but nothing wakes the screen (only a top screen with `SCR_KEEPAWAKE` does, `nos_main.c:2095`). `nos_t` does not advance while OFF (see [[Engine and scheduling]]), so the banner appears when the screen next wakes (inferred; not exercised).
- Files: `nos/svc.c:51-64,172-275,1021-1042,1256-1261`, `nos/svc.h:30-52`, `nos/apps/sys_notifs.c`, `core/notif.c`.

### svc-calls: call state and ring screen (nos/svc.c, nos/apps/con_call.c)

- `gb_call` (`svc.c:1044`): `cmd` (16 B), `name` (32 B), `number` (24 B). `incoming` -> state 1 (ringing), new generation if the number/name differs or the state was not already 1, name falls back to number then "Unknown". `outgoing`/`start`/`accept` -> state 2 (active), `t0_ms` set on entry to 2. `end`/`reject`/`missed`/`ignore` -> state 0. Case-insensitive (`ieq`). A second `incoming` while a call is active overwrites to ringing (no call-waiting model). No notification is created for a missed call (Gadgetbridge posts one as a normal `notify`).
- Ringer: `ringer_tick` (`:688`) priority find-watch > call > alarm > timer; every 2000 ms `hal_haptic(HAPTIC_ALARM)` (not gated by `nos_prefs.haptics` or DND) and `hal_sfx(SFX_NOTIFY)` only if `nos_prefs.sound` for calls; `nos_keep_awake()` while any ring is active. Alarm and timer rings self-stop at `RING_MAX_MS=60 s`; a ringing call has NO timeout.
- Screen: `ring_screen(&R_CALL, ...)` pushes `scr_call` (flags `SCR_NOBACK|SCR_KEEPAWAKE`, `con_call.c:499`), so it wakes the display from OFF within one service tick. After the call ends it gets a 2.5 s grace (`RING_GRACE_MS`) then the service pops it. Swiping the call screen away while ringing sets state 0 locally and sends nothing ("leave the call to the phone", `svc.c:977-980`).
- Actions out: Accept button -> `{"t":"call","n":"ACCEPT"}` and local state 2 immediately; Decline -> `REJECT` when state 1 else `END`; End -> `END`; each sets local state 0/2 before any phone confirmation (`svc.c:943-964`, `con_call.c:463-485`). Physical button on an incoming call only silences the ringer (`svc_ring_silence`, no message).
- UI follows the phone: `call_tick` goes to ENDED (shows "missed" if it was ringing) when `svc_call()->state==0` for more than 0.3 s (`con_call.c:206-209`).
- Disconnect: state forced to 0, but queued ring-buffer lines are processed after the edge (see Gaps).
- Files: `nos/svc.c:940-964,1044-1071`, `nos/apps/con_call.c`. See [[Utility apps]] for ring screens.

### svc-music: now playing over NUS (nos/svc.c, nos/apps/con_music.c)

- `musicinfo`: copies `track`/`artist`/`album` (sizes of `svc_music_t`: 64/48/48), `dur` seconds -> `dur_ms`; first info from GB resets `playing=0`, position 0, `volume=-1`; sets `have=1`, `s_music_src=1`. `musicstate`: `playing = (state=="play")`, `position` (seconds, -1 -> keep current) via `music_set_pos`; position interpolated locally in `music_tick` while playing (clamped to `dur_ms`). A `state` other than "play" (pause, stop) maps to paused. Companion (BLE media bridge) is the fallback source (`s_music_src==2`, polled at 4 Hz); GB wins whenever `musicinfo/musicstate` arrive.
- Out: `svc_music_cmd` sends `{"t":"music","n":"play|pause|next|previous|volumeup|volumedown"}` when `hal_phone_connected() && s_music_src != 2`; PLAYPAUSE flips `s_music.playing` immediately ("the phone confirms with musicstate"); otherwise `hal_media_cmd(cmd)` to the companion. The trace result is left `DIAG_RESULT_PENDING`.
- Volume: UI drag converts to N x 10% `volumeup/volumedown` lines in one tick (`con_music.c:715-725`); `svc_music().volume` stays -1 for GB (no volume read-back), so the volume bar is a local estimate (`M.vol`).
- Link UX: after a fresh link the player holds for 1 s (`P.waiting`) so it does not flash "Nothing playing" (`con_music.c:93-103`). `P.want` optimistic play state is released when the phone echoes or after 2 s.
- Disconnect clears GB music; the app shows the not-connected/not-playing page. See [[Connectivity and voice]].
- Files: `nos/svc.c:276-334,1156-1174`, `nos/apps/con_music.c`.

### svc-find: Find Phone and Find Watch (nos/svc.c, nos/apps/con_find.c)

- Find phone (watch to phone): tap sends `{"t":"findPhone","n":true}` (false to stop) when linked; if not on NUS, `hal_find_phone()` asks the PC companion (`0x07` on MEDIA_CMD). `s_find_phone` is only ever set by the watch itself and cleared by the watch or on disconnect; the phone's own stop is never observed, so `scr_find`'s "phone may stop on its own" branch (`con_find.c:253`) can never trigger from GB. Leaving the screen sends `findPhone false`. `find_tick` also cancels if the link drops.
- Find watch (phone to watch): `find` with `n` truthy sets `s_find_watch=1` and records `s_find_watch_t0` only on a rising edge; `n:false` clears it. `call_find_tick` pushes `scr_find_watch` (flags `SCR_NOBACK|SCR_KEEPAWAKE`, wakes the display), sets brightness override 255 (`con_find.c:421`), rings via `ringer_tick` (2 s haptic+sfx, haptic ungated), auto-stops after 2 x `RING_MAX_MS` = 120 s (`svc.c:981`). Tap/button/long-press -> `svc_find_watch_stop()`: local only, nothing is sent back to Gadgetbridge. A swipe-away is also treated as stop. `s_find_watch` survives a disconnect until tap or the 120 s timeout.
- `vibrate`: `hal_haptic(HAPTIC_BUZZ)` unconditionally (ignores `haptics` pref and DND); parameters ignored.
- Files: `nos/svc.c:965-983,1175-1178,1188`, `nos/apps/con_find.c`.

### svc-weather: phone weather (nos/svc.c)

- `gb_weather` (`svc.c:1073`): wipes `s_wx`, `source=1`, `generation++`, `temp/feels/hi/lo` via `kelvin_c` (values above 150 are treated as Kelvin, otherwise already Celsius), `code` mapped by `wx_from_owm`, `txt` sentence-cased, `hum`, `rain`, `uv`, `wind`, `wdir`, `loc`. Exactly one forecast day (today) is created; the code deliberately does not merge the Wi-Fi hourly/daily forecast (comment at `:1102`; STALE: `docs/PHONE.md` says the forecast comes from Wi-Fi). Not persisted. `s_wx_gb_ms = hal_millis()|1`. `wx_tick` (every 10 s) keeps phone weather while fresher than 3 h (`stale` flag after 1 h), then falls back to the Wi-Fi weather cache. See [[Services hub]].

### svc-alarms: phone alarm list (nos/svc.c)

- `gb_alarms` (`svc.c:1112`): requires `d` to be an array (otherwise ignored). It stops a ringing alarm, cancels any snooze, ZEROES the whole table, then rebuilds from `d` (max `SVC_ALARM_MAX = 8`, extra entries dropped silently; items with `h/m` out of range skipped), `rep` bit0=Monday...bit6=Sunday converted to the watch's Sunday=bit0 mask, `rep==0` means one-shot, `on` default 1, label forced to "Phone". Then `svc_alarm_save()` -> NVS namespace `wos`, key `nosalarm` via `hal_store_save_checked` (blob `alarm_saved`, `ALARM_VERSION 1`, `hal_esp32.c:177,236`). An empty `d:[]` deletes every alarm, including ones created on the watch. No acknowledgment, no upload of watch alarms to the phone.
- Files: `nos/svc.c:707-742,1112-1138`. See [[Storage and persistence]].

### svc-activity-gb: realtime activity reply (nos/svc.c)

- `act` (`svc.c:1180`): `hrm`, `stp` booleans, `int` seconds default 10 clamped 1..600; interval is active only if hrm or stp is requested; `s_act_rt_steps` baselined; replies immediately with `{"t":"act","stp":<delta since last send>,"hrm":<bpm_last or 0>,"rt":1}` (`gb_act_send`, `:1140`) then every `int` s from `phone_tick` (checked per tick, so at 1 Hz granularity with the screen off). `hrm` is `s_heart.bpm_last` with no age limit. The stream stops on disconnect (`s_act_rt_int=0`) and is not resumed on reconnect; the phone must send `act` again. No `actfetch` history. See [[Health and activity]].

### svc-track and the GPS handshake (nos/svc_track.c)

- `is_gps_active` always answers `{"t":"gps_power","status":false}` (`svc.c:1189`). The watch never sends `gps_power true`, never requests phone GPS and ignores any `gps` message. `svc_track` is independent of the phone: `svc_track_tick` (called from `svc_tick` every frame, `svc.c:1308`) polls `hal_gps_get` (the strap UART GPS in `firmware/main/ext/ext_gps.c`), calls `hal_gps_enable(1)` each frame while recording, rejects fixes (unavailable, invalid, age >3000 ms, out of range, HDOP >10 or <0, outlier >500 m or >0.08 m/ms), keeps up to `SVC_TRACK_MAXPTS` points in PSRAM (decimates by 2 when full) and saves GPX atomically (`.part` + rename) on stop in a `track_save` task (prio 1, stack 4096) under the SD lock to `/sdcard/watch/tracks/NNNN.gpx` (`svc_track.c:205-262,299`). So a recording in progress is reported to Gadgetbridge as "GPS not active". See [[Sports and workouts]] and [[Hardware and drivers]].

### svc-time-sync: setTime and E.setTimeZone (nos/svc.c)

- `apply_time` (`svc.c:1000`): tz from `E.setTimeZone` (hours x 60, rounded) else `nos_prefs.tz_min`; rejects tz outside -720..840 min (aborts before setting time). Changed tz updates `nos_prefs.tz_min` and `prefs_save()` (SD authoritative settings, NVS fallback; see [[Storage and persistence]]). Always `hal_set_tz_min(tz)` (`hal_esp32.c:833`, applies `TZ` env). If `setTime` carries an epoch >= 946684800 it converts to local civil time and calls `hal_set_datetime`. Runs on every such line; there is no rate limit or sanity check against the RTC.

### ext-phone-gadgetbridge (external)

- Phone-side app (Gadgetbridge or "Bangle.js Gadgetbridge") configured per `docs/PHONE.md`. Behaviour inside Gadgetbridge (auto-reconnect policy, what it sends on connect, id assignment) cannot be verified from this repo; statements about it here are limited to what the watch code assumes.

## How it runs

| Layer | Context | Priority/core | Cadence | Screen off |
|---|---|---|---|---|
| GATT RX writes, line assembly, ring push | `nimble_host` task (`hal_esp32.c:1531`) | core 0, prio 21, stack 4096 | event driven | runs |
| Host control callout `ble_control_tick` | NimBLE eventq, same task | core 0 | 100 ms, self re-arming, runs even when BLE is logically off | runs |
| `radio_poll` (BLE init retry) | UI task via `hal_setup_poll` | core 0 | every loop; 5 s retry backoff | runs |
| `radio_preferences` | UI task | core 0 | every loop | runs |
| Ring pop, JSON, handlers, TX | UI task (`app_main`, task "main", prio 1, raised in ON frames) inside `svc_tick` | core 0 | per frame ON; 1 Hz OFF (dt=1.0); 1 Hz AOD | runs, 8 lines/s max |
| `ver` | `phone_tick` | UI | 500 ms after the NUS-subscribed edge, seen at tick granularity | runs |
| `status` | `phone_tick` | UI | 600 s, or 5 % change, or charger change | runs |
| Realtime `act` | `phone_tick` | UI | `int` s (1-600) after `act` request | runs |
| Ring/haptic repeat | `ringer_tick` | UI | every 2 s while a ring is active | runs; call/find wake the display |
| Phone weather freshness | `wx_tick` | UI | 10 s; 3 h freshness | runs |
| NVS writes | UI task, `hal_store_save_checked` | n/a | on `alarm` message and tz change | runs |
| GPX save | `track_save` task (created per stop) | prio 1 | once per stop | runs |

Order inside `svc_tick` (`svc.c:1303-1329`): track, memo, cell, `phone_tick` (ver/status and up to 8 lines), `companion_tick`, `music_tick`, 1 s slow block (sys, activity, sports), workout, HR, weather, stopwatch, timer, alarm, call/find rings, ringer. Handlers therefore always run before the ring screens react in the same tick. See [[Engine and scheduling]] and [[Power and battery]] (10 Hz host callout and 8 lines/s screen-off drain are the relevant power/latency choices).

Ordering of the NUS bytes: `GB(...)` lines are processed strictly FIFO; the ring has no priority lanes, so a burst of bulky notifications delays a call or `find` message behind them.

## Data in / data out

Phone to watch (all via NUS RX; handler in `gb_message`, `svc.c:1148`):

| `t` | Handler and fields read | Effect | UI/service effect | Reply |
|---|---|---|---|---|
| `notify` | `gb_notify`: id, src, title/sender/subject, body | store (20), unread | banner + buzz unless DND; list/detail | none |
| `notify~` | `gb_notify(change)`: id, title, body | in-place edit, `read=0` if id exists and phone-origin | list refresh | none |
| `notify-` | id | remove | list/banner target validity | none |
| `call` | cmd, name, number | state 0/1/2 | `scr_call` push/pop, ringer | none |
| `musicinfo` | track, artist, album, dur | `s_music` | `scr_music` | none |
| `musicstate` | state, position | play flag, position base | `scr_music` | none |
| `weather` | temp, feels/feel, hi, lo, code, txt, hum, rain, uv, wind, wdir, loc | `s_wx` source 1 | weather app/face widgets | none |
| `find` | n (bool) | `s_find_watch` | `scr_find_watch` wakes screen, 120 s cap | none |
| `alarm` | d[] {h,m,rep,on} | replace all alarms, persist | alarm app/ringer | none |
| `act` | hrm, stp, int | realtime stream on/off | none | immediate `act` then periodic |
| `vibrate` | (none used) | `HAPTIC_BUZZ` | | none |
| `is_gps_active` | (none) | | | `gps_power false` |
| `setTime(..)` / `E.setTimeZone(..)` statements | epoch, hours | RTC + tz pref | all clocks | none |
| `calendar`, `nav`, `gps`, `http`, `actfetch`, unknown | | ignored | | none |

Watch to phone (`hal_phone_write_line`, each terminated `\r\n`):

| Line | Trigger | Code |
|---|---|---|
| `{"t":"ver","fw":"NOS 0.5.0-alpha","hw":"ESP32-S3 AMOLED 1.43"}` | 500 ms after link edge | `svc.c:1231` |
| `{"t":"status","bat":N,"volt":V.VV,"chg":0\|1}` | link edge, 10 min, 5 % step, charger edge | `svc.c:167` |
| `{"t":"notify","id":N,"n":"DISMISS"}` | swipe/dismiss/clear of a phone-origin entry while linked | `svc.c:248,255` |
| `{"t":"notify","id":N,"n":"OPEN"}` | Open on phone | `svc.c:272` |
| `{"t":"music","n":"play\|pause\|next\|previous\|volumeup\|volumedown"}` | music UI | `svc.c:299` |
| `{"t":"call","n":"ACCEPT"}` / `REJECT` / `END` | call buttons | `svc.c:947,955,962` |
| `{"t":"findPhone","n":true\|false}` | Find Phone tap, leave screen | `svc.c:969` |
| `{"t":"act","stp":N,"hrm":N,"rt":1}` | `act` request and interval | `svc.c:1145` |
| `{"t":"gps_power","status":false}` | `is_gps_active` | `svc.c:1189` |

Sizes and limits:

| Item | Value | Source |
|---|---|---|
| Max line accepted | 2047 B (`GB_LINE_MAX 2048`) | `gb.h:16`, `hal_esp32.c:1082` |
| RX ring | 8192 B, `[len:2]` records | `hal_esp32.c:1083` |
| JSON tokens / depth | 192 / 24 | `gb.h:39`, `gb.c:243` |
| Lines drained per tick | 8 | `svc.c:49` |
| Notification store | 20 x (app 20, title 40, body 160) | `svc.h:32-42` |
| Alarm table | 8 | `svc.h:119` |
| TX chunk | min(MTU-3, 180) B; 20 B at MTU 23 | `hal_esp32.c:1198-1199` |
| TX line buffer | 256 B | `svc.c:153` |
| Companion notify queue | 8 x {app 20, title 40} | `hal_esp32.c:2324` |

## Persistence

| Data | Where | Flush |
|---|---|---|
| Alarms from `alarm` | NVS namespace `wos`, key `nosalarm` (`alarm_saved`, version 1) | immediately in `gb_alarms` (`svc_alarm_save`) |
| Timezone | `nos_prefs.tz_min` in the SD `/watch/settings.N.bin` checkpoints (NVS `nosprefs` mirror) | `prefs_save()` when tz changes |
| Realtime activity baseline | none (RAM `s_act_rt_*`) | not resumed after reconnect |
| Notifications, music, call, weather, find state | RAM only | lost on reboot |
| BLE bond/keys | none (`sm_bonding=0`, RAM store, NVS persist off) | n/a |
| NUS counters | RAM, never read | n/a |

See [[Storage and persistence]].

## Interactions

- [[Services hub]]: `svc_tick` owner, getters used by apps.
- [[Connectivity and voice]]: BLE host, companion GATT service, `BLE_XFER`, Wi-Fi weather fallback.
- [[Engine and scheduling]]: service tick cadence in ON/AOD/OFF, ring-screen wake via `SCR_KEEPAWAKE`, `nos_t` not advancing while OFF.
- [[Shell and faces]]: banner, `sys_notifs.c`, home notification counts.
- [[Utility apps]]: music, find phone, alarms, call screen.
- [[Health and activity]]: `act` realtime stream, no `actfetch`.
- [[Sports and workouts]]: `svc_track` GPX recorder (independent of GB).
- [[Power and battery]]: 10 Hz host callout, 2 s ring haptics, battery `status` reporting.
- [[Storage and persistence]]: `nosalarm`, `tz_min`.
- [[Settings and developer]]: BLE toggle (`nos_prefs.ble`), `gb <line>` serial injection (`nos_main.c:1594`), "Connected" labels.
- [[Hardware and drivers]]: strap GPS, haptics, radio.
- [[Gaps]], [[OS overview]].

## Gaps

| # | Severity | Gap | Evidence | User impact |
|---|---|---|---|---|
| 1 | medium | Unauthenticated link. No bond, no encryption, no allow-list; any central that connects and subscribes NUS TX becomes "the phone" and can write RX lines: fake notifications and calls, `alarm` with `d:[]` wipes every alarm, `setTime` rewrites the clock, `find`/`vibrate` buzz the watch. The `BLE_XFER` service on the same connection exposes SD commands (`sdput!`, `sdrm`, `sdls`, `sdget`) without authentication. | `hal_esp32.c:1558-1560,1133-1145`; `nos/svc.c:1148-1191`; `firmware/main/sd_xfer.c:534-560` | A nearby device can spoof or erase data. |
| 2 | medium | `alarm` replaces the whole alarm table, even with an empty `d` array, and persists immediately; watch-created alarms are deleted whenever Gadgetbridge syncs alarms. Extra phone alarms beyond 8 are silently truncated. | `nos/svc.c:1112-1138` | Alarms set on the watch can vanish after a phone sync. |
| 3 | medium | All receive-side loss is silent and unobservable: overlong lines (>2047 B), ring full (newest dropped), token/depth overflow, NUL truncation, bad JSON, unknown types. `s_nus_dropped`/`s_nus_lines` are written but never read, printed or logged; the phone gets no ATT error. | `hal_esp32.c:1092,1105-1130,1133-1145`; `nos/svc.c:1198-1199`; grep shows no reader | A lost `end`/`notify-`/`musicstate` leaves stale state; debugging requires guessing. |
| 4 | medium | NUS ring is not flushed on disconnect. After the link edge handling clears the call, queued lines are still read (read loop runs before `if(!c) return`), so a queued `call incoming` after the drop re-enters ringing with no phone. A ringing call has no timeout (alarm/timer have 60 s), so it rings until the user acts. Likewise queued `find true` re-arms find-watch (120 s cap). | `hal_esp32.c:1412-1424,1163-1181`; `nos/svc.c:1215-1241,688-705` | Phantom call/find rings after a link loss (timing dependent; not reproduced). |
| 5 | low | `s_find_watch` is not cleared on disconnect; the watch keeps ringing up to 120 s with the phone gone. | `nos/svc.c:1221-1227,981` | Annoying ring after the phone leaves. |
| 6 | low | Dismissals made while disconnected are local only and never replayed. There is no resync of the notification table on reconnect (nothing sent on connect except `ver` + `status`), read state is never sent, and the store is RAM only. | `nos/svc.c:150-153,245-258,1229-1234` | Phone still shows notifications the watch dismissed; after reboot the watch list is empty. |
| 7 | low | Notification store is id-keyed with no sequence or tombstones: out-of-order `notify~`/`notify-` are lost, a late `notify` after `notify-` is never removed, missing `id` collapses to id 0, duplicate ids replace and re-buzz, and when 20 are held the oldest (possibly unread, possibly phone-origin) is evicted without DISMISS. | `nos/svc.c:185-234,1021-1042,1154` | Occasional ghost or missing notifications. |
| 8 | low | Under multi-connection (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`, `ble_control` re-advertises every 100 ms) a second central overwrites `g_med_conn` (NUS TX target) and any disconnect clears NUS state for both links. `docs/PHONE.md` still says the max is 1. Inferred; not tested on hardware. | `hal_esp32.c:1368-1375,1392,1412-1420`; `firmware/sdkconfig:753`; `docs/PHONE.md` pairing step 2 | Using the PC companion and Gadgetbridge together can drop or misroute phone traffic. |
| 9 | low | `is_gps_active` always replies false, even while `svc_track` records with the strap GPS; `gps_power true` is never sent and phone `gps` is ignored. | `nos/svc.c:1189-1190`; `nos/svc_track.c:97-164` | Gadgetbridge cannot know a track is being recorded and cannot supply phone GPS. |
| 10 | low | Find Phone never observes the phone's own stop: `s_find_phone` is set only locally, so the "phone stopped itself" branch is dead for GB and the button shows RINGING until tapped or the screen is left. | `nos/svc.c:965-972`; `nos/apps/con_find.c:253` | UI can show ringing after the phone stopped. |
| 11 | low | Optimistic actions without ack: call ACCEPT/REJECT/END change local state first, music play/pause flips locally, OPEN toasts "Opened", DISMISS removes locally. If TX fails (`s_nus_dropped++`) the UI is wrong with no retry; `txf` returns silently when not connected. | `nos/svc.c:943-964,290-300,269-274`; `hal_esp32.c:1207-1218` | Occasional mismatch between watch and phone. |
| 12 | low | Screen-off drain is 8 lines per second and notifications do not wake the screen. A sustained burst above 8 lines/s or a burst larger than 8 KB overflows the ring (gap 3). Incoming call and find do wake the display. | `nos/svc.c:49,1236-1241`; `firmware/main/nos_main.c:2091-2095` | Mostly theoretical; bursty sync while the watch is idle is the risk. |
| 13 | low | Only the first `GB(` in a line is parsed; payload scan ignores backtick quoting; closing `)` unchecked; a bare-JSON line needs a double-quoted `"t"`. | `nos/gb.c:103-136` | Concatenated multi-message lines lose all but the first message. |
| 14 | low | Battery `status` reports `battery_percent` even when invalid (default 50) and realtime `act` reports `bpm_last` regardless of age. | `hal_esp32.c:287`; `nos/svc.c:160-170,1145` | Gadgetbridge can display stale or default values. |
| 15 | low | UI task can stall up to 20 ms per TX chunk when NimBLE mbufs are exhausted (`vTaskDelay(1)` x 20, per chunk, per line); CLEAR ALL with 20 phone entries repeats this. | `hal_esp32.c:1207-1218`; `nos/svc.c:251-258` | Rare frame hitch while the BLE pool is starved. |
| 16 | low | `ble_control_tick` re-arms itself every 100 ms forever, including when BLE is toggled off (stack retained), costing 10 host-task wake-ups/s. | `hal_esp32.c:1368-1375` | Small standing power cost. |
| 17 | low | `core/notif.c` is built but unreferenced; three notification structures exist (core 8, hal queue 8, svc 20). | `firmware/main/CMakeLists.txt:18`; grep for `notif_push` | Dead code and conceptual duplication. |
| 18 | low | Stale docs: `docs/PHONE.md` (MAX_CONNECTIONS 1, ver string "NOS 1.0", weather forecast merge, `notify~` described as removing). | `docs/PHONE.md` (pairing step 2, "What works" table, watch-to-phone table); `nos/svc.c:1102,1231`; `nos/nos.h:27` | Misleading maintenance docs. |
