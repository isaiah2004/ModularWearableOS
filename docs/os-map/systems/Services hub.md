---
title: "Services hub"
tags: [nos, os-map]
updated: 2026-10-23
---

# Services hub

## Summary

The services hub is `nos/svc.c` (1329 lines) plus `nos/gb.c` (Gadgetbridge Bangle.js parser) and `nos/svc_track.c` (GPS track recorder). It is a set of file-static state structs that apps read through `const` getters (`svc_notif_get`, `svc_music`, `svc_weather`, `svc_activity`, `svc_heart`, `svc_alarm_*`, `svc_timer`, `svc_stopwatch`, `svc_call`, `svc_sys`, `svc_track_*`). Everything runs on one thread: the UI task (`app_main`, core 0). `svc_tick()` is the engine background hook (`nos_bg_tick = services_tick`, `nos/apps/registry.c:62-72`), called once per frame at the display frame rate (`nos/nos.c:2341`) and, with the display off, once per second from the host loop with `dt=1.0` (`firmware/main/nos_main.c:2094`). In AOD it runs through the 1 Hz `nos_frame_run` (`nos_main.c:2121-2125`). So alarms, timers, calls, phone lines, step counting, track recording and workout recording keep working with the screen off. Notifications do not wake the screen; only ring screens (`SCR_KEEPAWAKE`) do (`nos_main.c:2095`).

Data comes from four places: (1) the Gadgetbridge phone link, a NUS (Nordic UART) GATT service on the NimBLE host (`hal_esp32.c:1076-1229`), whose lines are parsed by `gb.c` and dispatched in `svc.c:gb_message`; (2) the older "companion" bridge (BLE `0xF2` notification frames, HTTP `POST /notify`, BLE media characteristics) through `hal_notification_poll` / `hal_media_*`; (3) HAL caches polled each second (`hal_activity_get` from the pedometer in `board.c`, `hal_power_get`, `hal_hr_history_samples` from the MAX30102 strap, `hal_weather_get` from the Open-Meteo task); (4) user actions from apps (alarm edit, timer start, `svc_heart_record_at`).

Persistence is thin. Only alarms (`nosalarm`) and activity day/week buckets (`nosact`) are persisted, both as NVS blobs in namespace `wos`; GPX tracks go to SD on stop. Notifications, music, weather, heart history, timer, stopwatch, snooze and calls are RAM only.

What goes to the phone: `ver`, `status` (battery), music/call/notification-action commands, `findPhone`, `act` realtime replies (with `rt:1`), `gps_power:false`. Not implemented: `actfetch` (historic activity upload), alarm upload, heart/sleep/workout/track upload, calendar, nav, http, gps. A sibling worktree (`C:/Users/isaia/.t3/worktrees/smartwatch2.0/t3code-a85cc679`, branch `t3code/fix-bangle-activity-tracking`) has the `actfetch` work but it is NOT in this workspace (`grep actfetch` finds only the comment `svc.c:1190`).

Legacy `core/notif.c` (8-entry store of `notif_t`) is compiled (`firmware/main/CMakeLists.txt:18`) but has no NOS caller; the real store is `svc_notif[20]` in PSRAM. See [[Gaps]] and [[OS overview]].

## Components

### svc hub core: svc_init / svc_tick (nos/svc.c)
- Purpose: allocate one PSRAM block `svc_big` (20 `svc_notif` + `phone[20]` flags + 2048 B GB line buffer + `gb_json` 192 tokens, about 10 KB) via `hal_big_alloc` (`svc.c:52-58,1281`), load persisted alarms/activity, and run every service.
- Runs: UI task. Per-frame order in `svc_tick` (`svc.c:1303-1329`): `svc_track_tick`, `svc_memo_tick`, `svc_cell_tick`, `phone_tick`, `companion_tick`, `music_tick`, then a 1 s gate (`sys_tick`, `act_tick`, `sports_tick`), `workout_tick` (every frame), `heart_tick` (60 s), `wx_tick` (10 s gate), stopwatch update, `timer_tick`, `alarm_tick`, `call_find_tick`, `ringer_tick`.
- `svc_init` also calls `tuning_init`, `sports_init`, `workout_init` (`svc.c:1298-1300`), so the Sports journal and workout engine are initialised by the hub ([[Sports and workouts]]).
- Reads: `nos_prefs` (`dnd`, `sound`, `tz_min`, `step_goal_100`), all HAL caches. Writes: all service structs, ring screens via `nos_push`/`nos_pop`, banners via `nos_banner`.
- Files: `nos/svc.c:1275-1329`, `nos/svc.h`, `nos/apps/registry.c:62-72`, `nos/nos.c:2341`, `firmware/main/nos_main.c:2094`.

### svc-gadgetbridge: gb.c framing, JSON, text folding
- Purpose: portable C, no allocation. `gb_frame_line` finds `GB({...})` (top-level only, skips quoted/commented text) or a bare JSON line, plus `setTime(n)` and `E.setTimeZone(h)` (`gb.c:54-149`). `gb_parse` is a tolerant tokenizer (unquoted keys, single quotes, `undefined`, depth 24, `GB_TOK_MAX` 192 tokens; overflow makes the whole message fail silently, `gb.c:156-159`). Getters `gb_int/gb_numk/gb_bool/gb_strk`. `gb_ascii` folds UTF-8/Latin-1/`\xNN`/`\uNNNN` to ASCII for the dot fonts (accents folded, smart quotes, emoji dropped, unknown becomes `?`).
- Runs: UI task, called from `handle_line` (`svc.c:1193-1200`). Up to `LINES_PER_FRAME`=8 lines per tick (`svc.c:1236`), so 8 lines/s with the screen off.
- Files: `nos/gb.c`, `nos/gb.h`, tests `tests/gb_frame_test.c`, `tests/test_gb_framing.py`, `tools/gb_test.c`, `tools/gb_rx_check.py`.

### svc-phone-link: NUS transport (firmware/main/hal_esp32.c)
- Purpose: bytes only. RX characteristic writes are split into lines by the NimBLE host task (`nus_rx_bytes`, `\n` terminates, `\r` ignored, lines over 2048 B dropped) and pushed into an 8192 B PSRAM ring under a mutex (`nus_push_line`, `hal_esp32.c:1096-1130`). The UI task pops one line per call with a zero-timeout lock (`hal_phone_read_line`, 1163). `hal_phone_write_line` chunks to MTU-3 (max 180 B), retries up to 20 ticks on mbuf exhaustion by `vTaskDelay(1)` (1190-1228), always terminates with `\r\n` (Gadgetbridge drops the byte before `\n`), and leads with an EOL after a cut or a new subscription.
- `hal_phone_connected()` = BLE enabled AND control clock ready AND TX notifications subscribed AND `g_med_conn` valid (1159-1162). Single central: the companion media service and NUS share `g_med_conn`.
- Runs: producer in task `nimble_host` (core 0, priority `configMAX_PRIORITIES-4`, `hal_esp32.c:1531`), consumer on UI task. Disconnect clears the line assembler but not the ring (1419-1421).
- Counters `s_nus_lines` and `s_nus_dropped` are incremented but never read or printed (`hal_esp32.c:1092,1113,1114,1124,1212`).

### svc-notifications (nos/svc.c:172-274, 1021-1042)
- State: `svc_notif {id, app[20], title[40], body[160], t_epoch, read, icon, instance}` x 20 (newest first) in PSRAM, parallel `phone[]` flag, `s_notif_revision`, per-entry `instance` identity, banner identity (`s_banner_notif_id/instance`).
- Writers: `gb_notify` (`notify` adds, `notify~` edits title/body in place and marks unread, `notify-` removes), `companion_tick` (up to 4 per frame from `hal_notification_poll`: app and title only, body empty), `svc_notif_add_local` (sim only), `svc_notif_dismiss/clear/mark_all_read/open_on_phone`.
- Readers: `sys_notifs.c` (list, detail, clear), `sys_home.c:476` (unread count for faces), banner callback `banner_open` -> `sys_notif_open`.
- Trigger: a phone `notify` line or companion frame. `notif_post` replaces by id (update moves to top), assigns `instance`, folds text with `gb_ascii`, stamps local epoch (phone `ts` ignored), picks an icon by substring of the app name, and posts a banner unless `nos_prefs.dnd` (`svc.c:230-233`). The banner also plays the notify feedback (haptic, `nos.c:519`).
- Phone sync out: `{"t":"notify","id":N,"n":"DISMISS"}` on dismiss/clear of phone-sourced items, `"n":"OPEN"` for "open on phone". Sent only if connected (`txf`, `svc.c:152`), so a dismissal made while disconnected never reaches the phone.
- Persistence: none. Overflow drops the oldest silently.
- Wake: no screen wake on arrival; with the screen off the user gets the haptic only.

### svc-music (svc.c:276-334, 1156-1174)
- State: `svc_music_t {have, track[64], artist[48], album[48], playing, pos_ms, dur_ms, volume}`; source tag `s_music_src` (0 none, 1 Gadgetbridge, 2 companion BLE/Wi-Fi bridge).
- Writers: GB `musicinfo` (track/artist/album/`dur` s) and `musicstate` (`state`, `position` s); `music_tick` polls `hal_media_get/time` at 4 Hz when no GB source; `svc_music_cmd` optimistically toggles `playing`.
- Readers: `con_music.c` (player, ring, volume pill), `con_find.c`/`set_*` for connection state.
- Sync out: `{"t":"music","n":"play|pause|next|previous|volumeup|volumedown"}`; if no GB link or src==2, `hal_media_cmd` notifies the companion characteristic.
- Position is interpolated locally from `s_music_pos0 + (now - s_music_t0)`; reset to empty when the GB link drops (`svc.c:1226`).
- Volume: `volume` is always -1 (nothing parses a volume), so the volume pill in `con_music.c:348,509` shows a local estimate.

### svc-weather (svc.c:336-424, 1073-1110; firmware/main/weather_service.c)
- State: `svc_weather_t` (valid, source 1 phone/2 Wi-Fi, generation, age, stale, fetching, status, temp/feels/hi/lo, cond WX_*, text, humidity/rain/uv/wind/wdir, loc, 12 hours x temp/cond/epoch, 5 days hi/lo/cond/epoch).
- Writers: GB `weather` (Kelvin or Celsius auto-detected, OWM `code` mapped by `wx_from_owm`) replaces the whole struct with a 1-day forecast only (`ndays=1`, hourly empty); `wx_tick` every 10 s otherwise copies `hal_weather_get` (Open-Meteo via task `weather`, `wx_from_wmo`) and merges 12 h / up to 5 d forecast. Phone data wins for 3 h after the last message (`gb_fresh`), then the Wi-Fi feed silently takes over; `stale` = 1 h.
- Fetch task: `weather_task` (priority 2, stack 8192, unpinned) wakes each 1 s, needs Wi-Fi up, valid location (`weather_loc` NVS blob) and an RTC/NTP clock, fetches current+12 h+4 d, refreshes every 30 min (60 s retry) (`weather_service.c:88-171`). Wi-Fi path carries no humidity, rain, uv or wind (set to -1, `svc.c:405`).
- Readers: `out_weather.c` (app), `sys_home.c:477` (glance on faces).
- Persistence: none (only the location is stored). Phone path never sent back.

### svc-activity (svc.c:426-578)
- State: `svc_activity_t {steps, goal, km, kcal, active_min, hourly[24], week[7], stand_hours, week_valid, checkpoint_pending/errors/epoch}` published from `act_saved s_as` (`day`, previous six day totals, hourly bins, active minutes, last/max steps, valid-day bits).
- Writer: `act_tick` each second from `hal_activity_get` (pedometer in `board.c`, `valid` only with trusted calendar and a sample < 250 ms old). Day rollover by `YYYYMMDD` key (`act_rollover`, gap capped at 7, clock-jump tolerant); hourly bins add the positive step delta in the current hour; active minute = more than 60 steps in a wall-clock minute; stand hour = bin > 250. `km = steps*0.75/1000`, `kcal = steps*0.04` (constants, no profile).
- Persistence: NVS blob `nosact` (92 B, version 2, v1 migration). Saved on rollover and every 10 min while dirty, 5 s retry after a failed save, via `hal_store_save_checked` on the UI task (`svc.c:496-509,558`). Not flushed in the shutdown or OTA path (`shutdown_tick` flushes prefs and chat history only, `nos_main.c:776-802`). The separate step counter itself is saved by `board-fs` every 60 s (`board.c:905-918`), so totals survive; hourly bins, active minutes and the week table lose up to 10 min.
- Readers: `fit_activity.c`, `fit_energy.c:144` (burn), `sys_home.c:469` (face data), faces `face_inverted/segment/split`, `background.c:130` (job samples BG_STEPS/BG_CALORIES), `sports.c:390`. See [[Health and activity]] and [[Calorie trace]].
- Phone sync: realtime only (below).
- Sim: `act_tick` is replaced by a canned believable day (`svc.c:457-478`).

### svc-heart (svc.c:580-644)
- State: `svc_heart_t {bpm_last, bpm_last_epoch, resting, min_today, max_today, hist[144], nhist, spo2_last}`.
- Writers: `heart_tick` every 60 s rebuilds `hist` (10-min bins, newest last) from `hal_hr_history_samples` (the MAX30102 strap `ext_hr.c` background ring of 144 samples, 1 per 10 min, first sample 2 min after boot, RAM); `svc_heart_record_at` from `fit_heart.c:554,663` and `fit_breathe.c:353` (bpm 35..230, rejects older acquisition epochs, updates min/max for today); `svc_heart_sample_epoch` converts monotonic age to epoch (3 s max age).
- Readers: `fit_heart.c`, `fit_breathe.c`, `sys_home.c`, `face_split.c`.
- Persistence: none (history ring, today min/max `s_hr_rec_day_*`, last reading all lost on reboot). Not sent to the phone except as the `hrm` field of a realtime `act` reply, with no staleness check (`svc.c:1145`).
- No strap fitted means all values stay 0.

### svc-alarms (svc.c:707-853, 1112-1138)
- State: `svc_alarm {h, m, days (bit0=Sunday, 0=once), on, label[16]}` x 8, ids and generation counter for edit-race detection, `s_ring_alarm`, snooze (`s_snooze_idx`, `s_snooze_at` in `hal_millis`).
- Writers: Alarms app (`time_alarm.c`) through `svc_alarm_add/remove/save/get`; GB `alarm` message replaces the whole table (days remapped from Monday-bit0 to Sunday-bit0, `label="Phone"`, then saved).
- Trigger: `alarm_tick` every frame; fires on the first tick of a matching minute (`key = day*1440+h*60+m`), one-shot alarms set `on=0` and save; snooze is 9 min (`SNOOZE_MS`); rings at most 60 s (`RING_MAX_MS`), then stops with no re-ring and no missed notice. Pushes `scr_alarm_ring` through `ring_screen`.
- Persistence: NVS blob `nosalarm` (164 B, version 1) via `svc_alarm_save`; a save failure is only traced, never retried (`svc.c:728-731`). Snooze state, ring state and ids are not persisted. Version mismatch silently loads zero alarms.
- Readers: `time_alarm.c`, `sys_home.c:482` (next alarm for faces). Alarms fire only while the MCU runs (screen off is fine); the shutdown mode uses deep sleep with only GPIO0 as wake source (`nos_main.c:810`), and nothing programs the PCF85063 alarm (`PIN_RTC_INT` is defined, `pins.h:56`, but unused for alarms).

### svc-timer (svc.c:855-910)
- State: `svc_timer_t {state 0 idle/1 running/2 paused/3 ringing, total_ms, left_ms}`, end time `s_timer_end` in `hal_millis`, generation.
- Writers: `time_timer.c` (`svc_timer_start/pause/resume/cancel/add`). `timer_tick` updates `left_ms` every frame, enters ringing at 0, auto-cancels after 60 s ringing, pushes `scr_timer_ring`. RAM only; lost on reboot.

### svc-stopwatch (svc.c:912-938)
- State: `svc_stopwatch_t {running, elapsed_ms, nlaps, laps[30]}`; `s_sw_t0`. `svc_tick` refreshes `elapsed_ms` every frame while running. Writers/readers: `time_stopwatch.c` only. RAM only; laps stop at 30 silently.

### svc-calls (svc.c:940-983, 1044-1071)
- State: `svc_call_t {state 0/1 incoming/2 active, name[32], number[24], t0_ms}` + generation.
- Writers: GB `call` (`incoming`, `outgoing|start|accept`, `end|reject|missed|ignore`); local `svc_call_accept/reject/hangup`.
- Sync out: `{"t":"call","n":"ACCEPT|REJECT|END"}`.
- Pushes `scr_call` (`con_call.c`), which silences via `svc_ring_silence`. Swiping the ringing screen away drops the state without telling the phone. Link loss clears the call (`svc.c:1225`). No missed-call record is created.

### svc-find (svc.c:965-983, 1175-1179)
- Find phone: `svc_find_phone(on)` sends `{"t":"findPhone","n":true|false}` over NUS, else rings through the companion (`hal_find_phone`, BLE media code 7). State flag `s_find_phone` cleared on disconnect; used by `con_find.c`.
- Find watch: GB `find` `n:true` sets `s_find_watch`, pushes `scr_find_watch`, auto-stops after 120 s.
- Ringer: `ringer_tick` (`svc.c:688-705`) selects find > call > alarm > timer, fires `HAPTIC_ALARM` plus an SFX every 2 s, keeps the display awake. Alarm, timer and find SFX ignore `nos_prefs.sound`; call SFX respects it.

### svc-system (svc.c:985-997)
- `svc_sys_t {battery_pct, charging, rail_mv, battery_valid, phone_connected, wifi_up, steps_today}` refreshed every second from `hal_power_get`, `hal_status(HAL_FEAT_WIFI)`, `s_connected`. Readers: `sys_control.c`, `sys_power.c`, `set_battery.c`, `set_modules.c`, `set_settings.c`, `sys_notifs.c`, `con_find.c`, `con_music.c`, `out_weather.c`, `sys_home.c`. `steps_today` duplicates `svc_activity()->steps`.
- Phone sync out: `status {bat, volt, chg}` at connect+500 ms, then every 10 min or on a 5 % / charger change (`svc.c:1229-1253`); `ver {fw:"NOS <ver>", hw:"ESP32-S3 AMOLED 1.43"}` once per connect.

### svc-phone-time (svc.c:1000-1019)
- GB `setTime`/`E.setTimeZone` on every connect: converts to local civil time with the phone's offset and calls `hal_set_datetime`, which writes the system clock and RTC; a changed offset is saved to `nos_prefs.tz_min` with `prefs_save`. Applied on every occurrence, unconditionally, so it overrides NTP and a manually set zone.

### svc-track (nos/svc_track.c)
- State: struct `T`: PSRAM `svc_track_pt[4096]` (32 B each, 128 KB, allocated on the first start), `n`, active/gap/candidate flags, distance, session id, atomics `pending/saving/save_result`.
- Trigger: `svc_track_tick` once per frame (guarded by `nos_frame`), keeps `hal_gps_enable(1)`, takes a fix if valid, age <= 3 s, hdop <= 10, finite and in range. Adds a point only after >= 5 m, rejects jumps over max(35 m, 0.08 m/ms), splits segments after > 15 s gaps with a 1 s re-acquisition filter; when full it drops every other point. Emits `SP_ROUTINE` sports events on start/stop.
- Stop: `svc_track_stop` freezes the points and spawns task `track_save` (stack 4096, priority 1, unpinned) which takes the SD lease and sports SD lock, writes `/sdcard/watch/tracks/NNNN.gpx.part`, fsyncs, renames, and publishes the result atomically (`svc_track.c:205-310`). A failed save keeps the session pending for retry; `svc_track_discard` clears it.
- Readers: `out_nav.c`, `sys_home.c` (REC chip, `svc_track_label`), `set_modules.c`, `set_diag.c`; `nos_main.c:745` and `ota.c:65` refuse shutdown/OTA while active/pending/saving.
- Persistence: points exist only in PSRAM until stop; no periodic checkpoint. Nothing is sent to the phone.

### svc-memo and svc-cell (defined in apps, declared in svc.h)
- `svc_memo_*` live in `nos/apps/con_memo.c:509-535` (recording belongs to the HAL recorder; `svc_memo_tick` finishes a card-full recording and toasts; WAV files in `/sdcard/watch/memos`). The REC chip reads `svc_memo_active/elapsed_ms`.
- `svc_cell_tick` (`nos/apps/con_cell.c:83`) only closes the diagnostic trace of an SMS send via `hal_lte_sms_send_status`. There is no cellular service state in the hub: SMS inbox is read on demand in the Cellular app (`hal_lte_sms_list`), and incoming SMS or calls from the LTE strap never create a `svc_notif`.

### svc-companion bridge (firmware/main/hal_esp32.c)
- Older PC/phone bridge, still compiled: notification queue of 8 `{app[20], title[40]}` (`s_notifications`, `hal_esp32.c:397-422,2324`) filled by BLE write `0xF2 app|title` on the time characteristic (859-861) or HTTP `POST /notify` (1770-1795, `web_notify`); media metadata via BLE `MEDIA_INFO`; commands notified on `MEDIA_CMD`. `hal_find_phone` is code 7 on `MEDIA_CMD`. The USB `notify` command exists only in legacy `main.c:755`.

### host_request.c (nos/host_request.c)
- Parses the `req <8 hex id> <command>` envelope of USB serial commands and classifies SD-transfer verbs (`sdfeatures, sdls, sdget, sdget2, sdput, sdput!, sdcrc, sdrm`) so they bypass the command queue (`nos_main.c:899-902`). Relation to services: the serial command `gb <line>` (`nos_main.c:1594`) goes through that queue and calls `svc_phone_inject`, which shares the `B->line` buffer and therefore must run on the UI task (it does: the queue is drained in the UI loop, `nos_main.c:1994`). No other service involvement.

### core/notif.c (legacy)
- 8-slot `notif_t {app[20], title[40], accent}` ring with `notif_sequence`. Compiled into the firmware but referenced only by legacy `core/apps`, `face_pip.c`, `sim/main_win32.c`. Dead in NOS.

## How it runs

| What | Context | Cadence | Screen off |
|---|---|---|---|
| `svc_tick` | UI task (`app_main`, core 0) via `nos_bg_tick` | every frame while PWR_ON (frame cap/30 fps class); AOD 1 Hz via `nos_frame_run`; PWR_OFF 1 Hz from `nos_main.c:2094` with `dt=1.0` | yes |
| Phone lines | consumer `phone_tick`; producer `nimble_host` task | up to 8 lines per tick; so 8 lines/s off | yes |
| Alarm/timer/stopwatch/call/find | `svc_tick` | per tick; minute-key edge for alarms | yes; ring screens wake the display |
| Activity | `act_tick` | 1 s; NVS save on dirty after 10 min or rollover | yes |
| Heart rebuild | `heart_tick` | 60 s | yes |
| Weather merge | `wx_tick` | 10 s gate | yes |
| Weather fetch | task `weather` (prio 2, 8192 B) | 30 min / 60 s retry | needs Wi-Fi |
| Track | `svc_track_tick`; save in task `track_save` (prio 1) | per tick (1 Hz when off); on stop | yes |
| Battery to phone | `phone_tick` | connect+0.5 s, 10 min, 5 % or charger change | yes |
| Realtime `act` | `phone_tick` | the `int` seconds the phone asked for (1..600) | yes |

All blocking I/O in the hub that touches flash (NVS commit in `act_save`, `svc_alarm_save`, `prefs_save`) runs on the UI task. Services have no mutex because only the UI task touches their state; the only cross-task inputs are the NUS ring (mutex), the notification queue (FreeRTOS queue), the `track_save` atomics and the HAL caches (critical sections).

## Data in / data out

### Phone -> watch (messages parsed in `gb_message`, `svc.c:1148-1191`)

| GB message | Handling | Result |
|---|---|---|
| `setTime(n)`, `E.setTimeZone(h)` (not JSON) | `apply_time` | RTC and tz pref |
| `notify` | `gb_notify` | new/updated notification and banner |
| `notify~` | `gb_notify(change)` | in-place edit, marked unread, no banner |
| `notify-` | `notif_remove_at` | removal |
| `call` | `gb_call` | call state and ring screen |
| `musicinfo`, `musicstate` | inline | music state |
| `weather` | `gb_weather` | weather, 1-day only |
| `find` | inline | find-watch ring |
| `alarm` | `gb_alarms` | replaces alarm table, saved |
| `act` | inline | starts/stops realtime reporting (`hrm`, `stp`, `int`) |
| `vibrate` | `hal_haptic(HAPTIC_BUZZ)` | buzz |
| `is_gps_active` | answered `gps_power:false` | |
| calendar, nav, gps, http, intent, `actfetch`, anything else | ignored (`svc.c:1190`) | |

### Watch -> phone (`txf`, `svc.c:150-159`, only when `hal_phone_connected()`)

| Line | When |
|---|---|
| `{"t":"ver","fw","hw"}` | connect + 500 ms |
| `{"t":"status","bat","volt","chg"}` | connect, 10 min, 5 % or charger change |
| `{"t":"act","stp":<delta>,"hrm":<bpm or 0>,"rt":1}` | on `act` request, then every `int` s |
| `{"t":"notify","id","n":"DISMISS"|"OPEN"}` | dismiss/clear/open of phone notifications |
| `{"t":"music","n":...}` | music buttons |
| `{"t":"call","n":"ACCEPT"|"REJECT"|"END"}` | call buttons |
| `{"t":"findPhone","n":bool}` | Find Phone app |
| `{"t":"gps_power","status":false}` | reply to `is_gps_active` |

### Service readers

| Service | Written by | Read by |
|---|---|---|
| notifications | GB, companion, sim | `sys_notifs.c`, `sys_home.c`, banner |
| music | GB, companion, `svc_music_cmd` | `con_music.c` |
| weather | GB, `hal_weather_get` | `out_weather.c`, `sys_home.c` |
| activity | pedometer via `act_tick` | `fit_activity`, `fit_energy`, `sys_home`, faces, `background.c`, `sports.c` |
| heart | `ext_hr` history, `fit_heart`, `fit_breathe` | `fit_heart`, `fit_breathe`, `sys_home`, `face_split` |
| alarms | `time_alarm.c`, GB | `time_alarm.c`, `sys_home.c` |
| timer / stopwatch | `time_timer.c` / `time_stopwatch.c` | same plus ring screen |
| calls, find | GB, `con_call.c`, `con_find.c` | `con_call.c`, `con_find.c` |
| system | HAL | ten screens (list above) |
| track | GPS strap, `out_nav.c` | `out_nav.c`, `sys_home.c`, shutdown/OTA guards |

## Persistence

| Item | Medium | Key/path | When | Survives reboot |
|---|---|---|---|---|
| Alarms | NVS `wos` | `nosalarm` (164 B v1) | on add/remove/save, one-shot fire, GB sync | yes; not on SD |
| Activity buckets | NVS `wos` | `nosact` (92 B v2) | rollover; 10 min if dirty; retry 5 s | yes; not on SD |
| Step counter | NVS | `pedometer` (owned by `board.c`) | 60 s | yes |
| Weather location | NVS | `weather_loc` | on selection | yes (weather data no) |
| GPX tracks | SD | `/sdcard/watch/tracks/NNNN.gpx` | on stop only | yes once saved |
| Voice memos | SD | `/sdcard/watch/memos/*.wav` | recorder | yes |
| Notifications, music, weather, heart, timer, stopwatch, snooze, calls | RAM | none | none | no |

See [[Storage and persistence]] for the NVS budget and SD arbitration. Phone `tz_min` goes to the preferences store ([[Settings and developer]]).

## Interactions

- [[Engine and scheduling]]: `nos_bg_tick` hook, 1 Hz OFF/AOD host loop, ring-screen push/pop and `SCR_KEEPAWAKE`.
- [[Connectivity and voice]]: NUS GATT service, companion bridge, Wi-Fi weather task, LTE strap SMS.
- [[Health and activity]]: `svc_activity`, `svc_heart`, pedometer, wrist detect; [[Calorie trace]] for the 0.04 kcal/step constant.
- [[Sports and workouts]]: `sports_tick` (1 Hz) and `workout_tick` (per tick) are driven from `svc_tick`; `svc_track_start/stop` emit `SP_ROUTINE`.
- [[Power and battery]]: `svc_sys` battery fields, the phone `status` line, shutdown guard on active track.
- [[Storage and persistence]]: `nosact`, `nosalarm`, GPX files.
- [[Hardware and drivers]]: `hw-strap-hr`, `hw-strap-gps`, `hw-ble-radio`.
- [[Shell and faces]]: faces read `svc_activity`, `svc_heart`, weather glance, unread count, REC chip.
- [[Utility apps]]: alarms, timer, stopwatch, music, call, find, recorder, cellular, weather.
- [[Rendering pipeline]]: banners and ring screens are drawn by the engine; the hub only calls `nos_banner`/`nos_push`.

## Gaps

| # | Severity | Gap | Evidence | User impact |
|---|---|---|---|---|
| 1 | high | No historic activity sync. Only realtime `act` with `rt:1` is sent, which Gadgetbridge does not store; `actfetch` is ignored. The implementation exists only in the unmerged worktree `t3code-a85cc679` | `nos/svc.c:1145,1190` | Steps and heart rate never appear in Gadgetbridge's database |
| 2 | high | A recorded GPS track lives only in PSRAM until Stop; no periodic checkpoint | `nos/svc_track.c:158-163,273-310` | Reboot, crash, flat battery or OTA mid-activity loses the whole track |
| 3 | medium | Alarm rings 60 s then stops silently; no auto re-ring or missed-alarm notification | `nos/svc.c:848-851` | A sleeper misses the alarm |
| 4 | medium | Alarms cannot fire while the watch is in shutdown/deep sleep; the RTC alarm is never programmed | `firmware/main/nos_main.c:810`, `firmware/main/pins.h:56` | "Powered off" watch will not wake for an alarm |
| 5 | medium | Alarm save failure is traced but never retried or flagged (activity has retry logic) | `nos/svc.c:728-731` | Alarms silently lost at next reboot after a failed NVS write |
| 6 | medium | GB `alarm` message wipes the local alarm table; watch alarms are never uploaded | `nos/svc.c:1112-1138` | Alarms set on the watch can vanish after a phone sync (not verified on device) |
| 7 | medium | Activity bins and active minutes flushed only every 10 min and not at shutdown/OTA; `checkpoint_*` fields are never read by any UI | `nos/svc.c:558`, `nos/svc.h:99-100`, `firmware/main/nos_main.c:776-802` | Up to 10 min of hourly/active data lost on restart; save failures invisible |
| 8 | medium | NVS commits (`act_save`, `svc_alarm_save`) run synchronously in the UI frame | `nos/svc.c:499,728`, `firmware/main/hal_esp32.c:228-257` | Possible frame hitch at save time |
| 9 | medium | Notifications are RAM only; not resynchronised after reboot; no wake when one arrives with the screen off (haptic only) | `nos/svc.c:230-233`, `firmware/main/nos_main.c:2095` | Missed messages after restart; screen stays dark |
| 10 | medium | LTE strap SMS and calls never reach the notification store or banners | `nos/apps/con_cell.c:83`, grep shows no `hal_notification_post`/`svc_notif` in `firmware/main/ext` | SMS arrives silently unless the Cellular app is open |
| 11 | medium | Phone weather: no hourly/daily forecast (only today), `hi/lo` default to the current temperature, missing `temp` shows 0 C, humidity/rain/uv often absent; Wi-Fi feed has no humidity/wind/rain/uv | `nos/svc.c:1081-1109`, `405` | Weather app shows flat or empty forecast rows on the phone path |
| 12 | medium | Calories and distance are constants (0.04 kcal/step, 0.75 m stride) with no user profile or heart rate | `nos/svc.c:45-46,444-445` | Wrong energy and distance for most users; see [[Calorie trace]] |
| 13 | medium | Heart history, today min/max and last reading are RAM only; `bpm_last` is sent in `act` without an age check | `nos/svc.c:584-644,1145`, `firmware/main/ext/ext_hr.c:61` | HR trend disappears on reboot; stale bpm reported to the phone |
| 14 | low | Timer, stopwatch, snooze and ring state are not persisted | `nos/svc.c:855-938` | Lost on reboot |
| 15 | low | Music volume is never populated (always -1), play/pause is toggled optimistically without confirmation, position not reset on track change | `nos/svc.c:286,296-297,1156-1164`, `nos/apps/con_music.c:348,509` | Volume pill is a guess; UI can disagree with the phone |
| 16 | low | Unhandled GB messages: calendar, nav, gps, http, intent, `actfetch`; no notification actions or reply | `nos/svc.c:1190` | Features absent; messages dropped without a counter |
| 17 | low | `spo2_last` is cleared whenever a record arrives with spo2 outside 70..100 (Breathe passes 0) | `nos/svc.c:642`, `nos/apps/fit_breathe.c:353` | SpO2 reading disappears after a breathing session |
| 18 | low | Activity keeps only 7 days (`week[7]`); no sleep, no long history | `nos/svc.h:95-96` | No month/year views |
| 19 | low | Dismissing a phone notification while disconnected is never relayed (`txf` returns when offline) | `nos/svc.c:152,245-250` | Notification reappears/stays on the phone |
| 20 | low | `txf` runs on the UI task and `hal_phone_write_line` may `vTaskDelay` up to 20 ticks per chunk; `svc_notif_clear` can send 20 lines in one frame | `firmware/main/hal_esp32.c:1205-1214`, `nos/svc.c:251-258` | Possible UI stall when the BLE mbuf pool is exhausted |
| 21 | low | NUS ring not flushed on disconnect; drop counters `s_nus_lines/s_nus_dropped` never exposed; oversize/over-token messages are dropped silently | `firmware/main/hal_esp32.c:1092,1419-1421`, `nos/gb.c:156-159` | Stale lines replayed after reconnect; hard to diagnose lost messages |
| 22 | low | Phone `setTime` overrides NTP and tz preference on every connect with no hysteresis | `nos/svc.c:1000-1019` | Clock jumps by phone skew |
| 23 | low | Dead or redundant code: `core/notif.c` compiled but unused; stub fallbacks in `time_alarm.c:60-98` and `time_stopwatch.c:34-60` unreachable; `svc_heart_record` unused; `svc_sys.steps_today` duplicates activity; `svc_notif_add_local` only used by the simulator; `svc_alarm_get` returns alarm 0 for a bad index | `core/notif.c`, `nos/apps/time_alarm.c:60`, `nos/svc.c:644,720`, `nos/apps/sys_home.c:471` | Maintenance weight, no user effect |
| 24 | low | `ver` reports a hard-coded hardware name `ESP32-S3 AMOLED 1.43` rather than the B board identity | `nos/svc.c:44` | Gadgetbridge shows a generic hardware string |
| 25 | low | Weather is not cached across reboot; phone weather outlives the link for 3 h then silently falls back to Wi-Fi | `nos/svc.c:412-424` | Empty weather after boot until a fetch |
