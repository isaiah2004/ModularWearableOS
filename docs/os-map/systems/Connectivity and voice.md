---
title: "Connectivity and voice"
tags: [nos, os-map]
updated: 2026-10-23
---

# Connectivity and voice

## Summary

This subsystem is everything that moves data on or off the 1.43-B watch over a radio: BLE (NimBLE, Gadgetbridge over the Nordic UART Service, a media/time GATT service, and an SD file-transfer service), Wi-Fi (station, setup access point, HTTP server, SNTP, OTA upload), the Open-Meteo weather fetcher, the optional Quectel EG800AK cellular modem on the extension port, and the OpenRouter voice chat (compiled in only on the 1.43C board).

Key facts:

- Radio intent is a saved preference (`nos_prefs.wifi`, `nos_prefs.ble`, `nos_prefs.lte`). `radio_preferences()` in `nos_main.c` (about line 953) pushes it into the HAL every main-loop iteration. Screen state does not gate it, so radios keep running with the screen off. Defaults are wifi off, ble on, lte off (`nos/prefs.c:150`).
- Actual radio bring-up is deferred and heap-admitted (`radio_startup`/`radio_admit`/`radio_fail` in `hal_esp32.c:75`), retried every 5 s.
- There is no radio power policy beyond on/off. `nos_power.c` uses DFS 80-240 MHz and `light_sleep=0`.
- Voice chat does not exist on B. `voice_chat.c` is `#if CONFIG_WATCH_BOARD_143C`; on B the stubs report "Requires C-board microphone and speaker", but the Chat app is still registered.
- Weather over Wi-Fi has no location setter in NOS (only legacy `main.c` calls `hal_weather_set_location`), so it is dead on a fresh install; Gadgetbridge weather works.
- Several network entry points are unauthenticated (HTTP `/notify`, `/cmd`, `/media`, `/cover`; BLE SD transfer service), and BLE does no pairing or bonding.

## Components

### BLE host and GATT (svc-ble, task-nimble-host)

- Purpose: NimBLE peripheral (advertise, accept phone) plus observer/central (scan, connect to a puck or device). Hosts the Watch service (TIME, MEDIA_INFO, MEDIA_COVER, MEDIA_CMD), NUS, and the BLE_XFER service. Name `Bangle.js NOS` (`firmware/main/hal_esp32.c:57`).
- Where it runs: NimBLE host task pinned to core 0, 4096 B stack, NimBLE memory in PSRAM, max 2 connections (`firmware/sdkconfig.defaults`). Control via a 100 ms NimBLE callout `ble_control_tick` (`hal_esp32.c:1334-1375`). `hal_ble_set_enabled` (`hal_esp32.c:527`) only sets a flag; the callout services it.
- Cadence: advertising connectable/undirected with NUS UUID in the scan response (`hal_esp32.c:1312`). Scan is active, 10 ms window every 160 ms, 8 s duration, leased 5 s per `hal_ble_scan` call (`hal_esp32.c:1568`). Discovery table 24 entries in PSRAM (`hal_esp32.c:490`). Central connect has a 10 s timeout (`hal_esp32.c:649-663`). Post-connect parameters requested: 80-120 ms, latency 3, 8 s timeout (`hal_esp32.c:1401`).
- Reads: `nos_prefs.ble`; heap thresholds (20480 B internal, 16384 once host task exists, largest block 8192 B).
- Writes: notification queue (8 entries, `hal_esp32.c:396-423`), time/location (`g_loc`, `hal_set_datetime`, `hal_set_tz_min`), media info/cover (two PSRAM pages, 250x250 RGB565 = 125000 B, 15 s abandon), NUS RX ring (8192 B PSRAM, lines up to 2048 B), 0xF1 locate flag `s_locate_until`.
- Security: `sm_bonding=0`, `sm_sc=0`, IO_CAP NONE (`hal_esp32.c:1558-1560`); fixed static random address (`hal_esp32.c:1485`). No pairing.
- Files: `firmware/main/hal_esp32.c`, `firmware/sdkconfig.defaults`.

### Gadgetbridge phone link (svc-phone-link, svc-gadgetbridge)

- Purpose: "Bangle.js" Gadgetbridge protocol over NUS. Phone to watch: notify, notify~, notify-, call, musicinfo, musicstate, weather, find, alarm, act, vibrate, is_gps_active, setTime/E.setTimeZone. Watch to phone: ver (500 ms after connect), status (battery every 10 min or on a 5% or charger change), act (realtime steps/HR), notify DISMISS/OPEN, music commands, call ACCEPT/REJECT/END, findPhone, gps_power false.
- Where it runs: the main loop, from the `nos/svc.c` service tick; reads at most 8 lines per frame from `hal_phone_read_line` (LINES_PER_FRAME 8); TX buffer 256 B. NUS TX notifies are chunked to MTU-3, at most 180 B, with a 20 x 1-tick retry.
- Unhandled: calendar, nav, gps, http, actfetch (comment at `nos/svc.c:1190`).
- Time: `apply_time` stores tz in `nos_prefs.tz_min` (prefs_save) and calls `hal_set_tz_min`/`hal_set_datetime`.
- Reads: NUS RX ring, `hal_notification_poll` (4 per tick via `companion_tick`), media state.
- Writes: notification store, music/call state, weather cache (`gb_weather`, `nos/svc.c:1073`), time, prefs.
- Files: `nos/svc.c`, `firmware/main/hal_esp32.c`.

### Watch GATT service: time, media, locate (svc-music, svc-notifications)

- Purpose: non-Gadgetbridge companion path. TIME_CHR writes carry epoch, tz, lat/lon/city; frame 0xF1 is "ring the puck"; 0xF2 is a notification. MEDIA_INFO (write), MEDIA_COVER (chunked), MEDIA_CMD (notify, codes 0-7, battery-log 0xB1/B2/B3).
- Where it runs: NimBLE host task for writes; polled by `companion_tick` in the main loop.
- Gap: `hal_locate_active` is polled only by legacy `firmware/main/main.c:1180`, so 0xF1 does nothing in NOS.
- Files: `firmware/main/hal_esp32.c:1292` (WATCH_GATT).

### BLE XFR SD transfer (svc-ble)

- Purpose: "NOS XFR" service (UUID base `7f5e0001-...`): CTRL (write/notify, 160 B commands, queue depth 2) and DATA (stream buffer 8192 B). Commands: sdls, sdcrc, sdput, sdput!, sdget, sdget2, sdrm, sdfeatures. Same command engine as the USB serial transport.
- Where it runs: commands execute in the serial-command task via `ble_xfer_poll` (`nos_main.c:888`, 50 ms cadence). Connection interval drops to 6-12 units, latency 0 while transferring and returns to 80-120 ms after 5 s idle.
- Safeguards: protected paths (`watch/settings.*.bin`, `watch/wifi/settings.*.bin`, `imu/intent.*.bin`) cannot be removed; live IMU recording files cannot be removed; `.part`, `.sdxfer.txn`, `.sdxfer-old` names are reserved; restore puts validate the file (`firmware/main/sd_xfer.c:272-330, 532-606`).
- No authentication or bonding: any connected central can list, read, write and delete SD files outside the protected set.
- Files: `firmware/main/ble_xfer.c`, `firmware/main/sd_xfer.c`.

### Wi-Fi station and setup AP (svc-wifi, task-wifi-settings)

- Purpose: station join with stored credentials, background scan cache for the picker, setup AP, SNTP, HTTP server start.
- Where it runs: `wifi-settings` task, `xTaskCreate(wifi_worker, "wifi-settings", 6144, NULL, 2)` (`hal_esp32.c:2268`), Wi-Fi on core 0 per config. Loop: 100 ms queue wait, `air_supervise`, and a credentials checkpoint to SD/NVS 5 s after a successful join (retried while dirty). Wi-Fi init needs 24576 B internal free (16384 if the task exists) and a largest block of 8192 B.
- Retry: `esp_timer` "wifi_retry" with backoff 2 s doubling to 300 s (`hal_esp32.c:1852-1876`).
- Scan: async, 32 APs cached in PSRAM, cached for 8 s, 9 s watchdog. Country hard-coded to IN channels 1-13, manual policy (`hal_esp32.c:2291`).
- On STA got-IP (`hal_esp32.c:1971`): `web_start` (HTTP server), SNTP (pool.ntp.org), `hal_weather_refresh`.
- Setup AP (`hal_setup_set`, `hal_esp32.c:1885`): SSID `Watch-Setup-XXXX`, random password, WPA2, channel 1, max 2 clients, 600 s `esp_timer`, calls `ota_session_begin`.
- Wi-Fi air analyzer (promiscuous channel hop, 120 ms dwell, `hal_wifi_air_*`, `hal_esp32.c:2067-2243`) has no NOS UI caller.
- Reads: `nos_prefs.wifi`, `wifi_cred_t {ssid[33], pass[65]}` from SD/NVS.
- Writes: credentials (see Persistence), system time via SNTP.
- Files: `firmware/main/hal_esp32.c`, `firmware/main/watch_setup.inc`.

### HTTP server (svc-wifi)

- Purpose: local web endpoints for phone setup and companion use. Max 16 URI handlers, 3 sockets, 6144 B stack in PSRAM, recv_wait_timeout 12 s (`hal_esp32.c:1798`).
- Routes: `/` (setup page, gated by `setup_authorized`), `/wifi`, `/api/networks`, `/api/status`, `/api/settings` (gated), `/cover`, `/media`, `/cmd`, `/notify` (NOT gated), `/update`, `/ota`, `/ota_url` (token gated).
- `/api/status` exposes battery, uptime, psram, phone, sd and prefs. `/api/settings` queues brightness, auto_bright, dnd and aod, applied later in `hal_setup_poll`. Wi-Fi password rules: 8-63 chars or empty.
- Lifetime: started on every STA got-IP and never stopped when the setup session ends (`hal_esp32.c:1812-1831, 1979`).
- Files: `firmware/main/hal_esp32.c`, `firmware/main/watch_setup.inc`.

### Radio admission and polling (svc-wifi, svc-ble)

- Purpose: heap-gated start of Wi-Fi, BLE, weather. `radio_startup` (`hal_esp32.c:75-107`), `hal_esp32_start_radios`/`radio_poll` (`hal_esp32.c:2360-2365`), called from `hal_setup_poll`, which `nos_main.c:1987` calls every main-loop iteration. Failure retries after 5 s. `hal_esp32_radios_pending` and the `NOSRADIO` serial report (`hal_esp32.c:2409`) expose state.
- Thresholds: Wi-Fi 24576/8192 B, BLE 20480/8192 B, weather 12288/8192 B (internal free / largest block).
- Files: `firmware/main/hal_esp32.c`, `nos/nos_main.c`.

### Connectivity settings screen (sys-connectivity)

- Purpose: one screen for both Wi-Fi and Bluetooth, selected with `set_connectivity_open(bt)` (from `set_settings.c:2269` and the serial `network ble|wifi` command).
- Wi-Fi rows: enable toggle (`nos_prefs.wifi` + `prefs_save`), Phone setup, Refresh, Other network, AP list. Scans every 3 s while open.
- Bluetooth rows: phone-link instructions, device list, disconnect, refresh. There is no BLE on/off toggle here; `nos_prefs.ble` changes only in the Control Center (`sys_control.c:229`, T_BT).
- Diag action IDs 0x6101-0x6106.
- Files: `nos/apps/set_connectivity.c:45-54`, `nos/apps/sys_control.c`.

### Weather fetcher (svc-weather-fetch, task-weather, svc-weather)

- Purpose: Open-Meteo forecast fetch and parse.
- Where it runs: task "weather", `xTaskCreate(weather_task, "weather", 8192, NULL, 2)`, polling every 1 s (`firmware/main/weather_service.c`).
- Preconditions: valid config (lat/lon), `HAL_FEAT_WIFI` UP, valid epoch. Request: `https://api.open-meteo.com/v1/forecast?...current,hourly,daily...forecast_hours=12&forecast_days=4&timezone=auto&timeformat=unixtime`. 16 KB body cap, 12 s timeout. Refetch 1800 s after success, 60 s after failure, at least 10 s between attempts. Stale after 3600 s.
- Parser (`weather_parse.c`): cJSON with range checks; 5 hourly points (every other hour) and 4 days.
- Selection in `nos/svc.c` (`wx_from_hal`/`wx_tick`): Gadgetbridge weather preferred when fresh (under 3 h), else the Wi-Fi cache. `hal_location` falls back to BLE-provided `g_loc`.
- Persistence: NVS "weather_loc" (version 1, lat, lon, city[32]).
- Gap: `hal_weather_set_location` is called only from legacy `main.c:776`.
- Files: `firmware/main/weather_service.c`, `firmware/main/weather_parse.c`, `nos/svc.c`.

### OTA over the network (svc-ota)

- Purpose: network update paths. `POST /ota` raw .bin (async handler, task "ota-upload" 4096 B priority 5); `POST /ota_url` (task "ota_url" 8192 B priority 5, `esp_https_ota`); `GET /update` page.
- Session: 600 s window opened by `ota_session_begin` with a 32-hex-char token (`X-Watch-Update`) from `esp_random`. Policy blocking by `ota_poll` (workout, recording, voice chat active, prefs pending...) and a 180 s writer deadline. `esp_ota_end` verifies SHA-256; `ota_mark_valid` after 90 frames (`nos_main.c:2227`); `ota cancel` serial command (`nos_main.c:1206`).
- Dead in NOS: `ota_from_sd` (`/sdcard/update.bin`) and `ota_from_url` have only legacy `main.c:693-695` callers.
- Security: `url_valid` accepts plain `http://`; `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y`; no image signature check; the `/update` page leaks the token to anyone who can reach the AP/LAN while a session is live.
- Partitions: two `0x780000` OTA slots, `BOOTLOADER_APP_ROLLBACK_ENABLE`.
- Files: `firmware/main/ota.c`, `firmware/partitions.csv`.

### Cellular modem (svc-calls, task-ext, task-exta, task-extf, app-cellular)

- Purpose: Quectel EG800AK on UART0 (TX 43 / RX 44, 115200), PWRKEY GPIO18, driver `ext_lte.c`; UI `con_cell.c`.
- Where it runs: the "ext" task (`ext_sys.c:186`, `xTaskCreatePinnedToCoreWithCaps`, 4096 B stack in SPIRAM, priority 0, no affinity, loop wait up to 50 ms). FSM OFF/CHECK/KEY/BOOT/INIT/READY/PWROFF/KEYOFF/ABSENT. Init: ATE0, CMEE=1, CPIN?, CMGF=1, CSCS="GSM", CNMI=2,1,0,0,0, COPS=3,0. Poll CSQ, CEREG?, CREG?, COPS? round-robin every 1.25 s; inbox CMGL every 60 s and on +CMTI; send via CMGS (up to 60 s); power-off via QPOWD.
- Power behaviour: `lte_init` sets `probing=1` so the modem powers once every boot to read SIM and inbox. `L->enabled` auto-clears after `NOS_EXT_IDLE_MS` = 8000 ms with no `hal_lte_get`/`sms_list` caller. `nos_prefs.lte` is applied only when con_cell, the Control Center (`sys_control.c:234`) or set_modules opens; nothing applies it at boot.
- Features: SMS list (12 in driver `MAX_SMS`, 16 in UI), send up to 160 chars with GSM charset, canned quick replies (`k_replies`). No voice call, CLIP, data connection, or SMS notifications; unread count RAM-only.
- Files: `firmware/main/ext/ext_lte.c`, `firmware/main/ext/ext_sys.c`, `nos/apps/con_cell.c`.

### Call screen (sys-call)

- Purpose: UI for an incoming/active phone call using `svc_call*` (accept, reject, hang up). No call audio, text "ON PHONE". Flags `SCR_NOBACK|SCR_KEEPAWAKE`; pushed by the services, not launched by the user.
- Files: `nos/apps/con_call.c`.

### Voice chat engine (svc-voice, task-voice)

- Purpose: record, STT, LLM, TTS pipeline through OpenRouter (`https://openrouter.ai/api/v1/`). STT `openai/whisper-large-v3-turbo`, TTS `hexgrad/kokoro-82m`, chat models by tier LOW `openai/gpt-4o-mini`, MEDIUM `openai/gpt-5.4-mini`, HIGH `openai/gpt-5.4`. 16 kHz mono, 20 s max record.
- Where it runs: worker task "voice" (10240 B stack in PSRAM, priority 3). `voice_chat_maintenance` runs from the main loop (`nos_main.c:1986`) and before deep sleep. Config key NVS "voice_chat" (API key plus stt/tts/voice) is provisioned over USB serial (`chat wifi {...}` also C-only).
- Board: the whole file is `#if CONFIG_WATCH_BOARD_143C`. On B `voice_chat_start` returns 0 and the serial command prints `ERR chat requires C board`.
- Files: `firmware/main/voice_chat.c`, `firmware/main/voice_chat.h`.

### Voice audio (svc-voice)

- Purpose: ES8311 speaker codec (0x18) and ES7210 microphone codec (0x40) over I2S (MCLK 38, BCLK 39, WS 40, DOUT 41, DIN 42), audio LDO GPIO18, amplifier GPIO46. C only; B stubs return 0.
- Files: `firmware/main/voice_audio.c`.

### Voice history and policy (svc-voice)

- Purpose: `nos/voice_history.c` keeps a fixed checksummed text-only checkpoint (4 chats x 2 turns, 128 B input and 320 B reply per turn, under 4 KiB, `nos/voice_history.h`) with `voice_history_append/delete/metadata`. `voice_policy.c` budgets spend per tier in cents/minute with `voice_reserve_usd`.
- Persistence: NVS key "voice_hist". A failed verified save stays dirty.
- Files: `nos/voice_history.c`, `nos/voice_history.h`, `nos/voice_policy.c`.

### Chat screen (app-chat)

- Purpose: UI for voice chat. Registered as "Chat" in `nos/apps/registry.c:13` even on B. On B `view.available=0` and it shows "Voice chat needs the C board mic and speaker."; `leave` calls `voice_chat_cancel`.
- Files: `nos/apps/con_chat.c`, `nos/apps/registry.c`.

### Asset reader task (task-asset-read)

- Purpose: background SD asset reads (`asset-read` task, 6144 B PSRAM stack, priority 1), `hal_esp32.c:2422-2542`.
- Files: `firmware/main/hal_esp32.c`.

## How it runs

- Control flow: preferences, then `radio_preferences()` in the main loop, then `hal_*_set_enabled` flags, then `radio_poll` (heap-admitted init, 5 s retry), then worker tasks and NimBLE callouts. Every enable is a flag, so the main loop never blocks on a radio.
- Tasks: NimBLE host (core 0, 4096 B), `wifi-settings` (6144 B, priority 2), `weather` (8192 B, priority 2), `ota-upload`/`ota_url` (priority 5, per upload), `voice` (C only, priority 3), `ext` (priority 0, SPIRAM stack), `asset-read` (priority 1), ESP-IDF HTTP server (PSRAM stack 6144 B).
- Timers: `wifi_retry` esp_timer, 600 s setup session esp_timer, 100 ms NimBLE `ble_control_tick`, 5 s scan lease.
- Main-loop polls: `hal_setup_poll` (`nos_main.c:1987`), `voice_chat_maintenance` (`nos_main.c:1986`), `ble_xfer_poll` (`nos_main.c:888`, 50 ms), service tick and `companion_tick` in `nos/svc.c`.
- Screen-off: radios keep their saved intent (BLE advertising and phone link, Wi-Fi station, weather refresh, SNTP). Notifications and music arrive while dark. LTE only powers when a UI caller keeps `L->enabled` alive (8 s idle), so it effectively sleeps with the screen off after the boot probe.
- Power: no radio-specific policy. DFS 80-240 MHz, no light sleep, `PM_ENABLE`. See [[Power and battery]].
- OTA blocking: `ota_poll` refuses to start while a workout, recording, voice chat or pending prefs write is active. See [[Engine and scheduling]].

## Data in / data out

| Direction | Data | Source / sink | Path |
|---|---|---|---|
| In | Time, tz, location, city | Phone (BLE TIME char or Gadgetbridge setTime) | `hal_esp32.c`, `svc.c apply_time` |
| In | Notifications | Phone via NUS `notify`, BLE 0xF2, HTTP `/notify` | `svc.c`, `hal_esp32.c` queue (8) |
| In | Call state, music info, music state, cover art | Phone | NUS, MEDIA_INFO, MEDIA_COVER |
| In | Weather (Gadgetbridge) | Phone | `svc.c gb_weather` |
| In | Weather (Open-Meteo JSON, up to 16 KB) | Internet over Wi-Fi | `weather_service.c` |
| In | SNTP time | pool.ntp.org | `hal_esp32.c` |
| In | Firmware image | Host PC / URL over HTTP(S) | `/ota`, `/ota_url` |
| In | SD files, commands | BLE XFR / USB serial | `ble_xfer.c`, `sd_xfer.c` |
| In | Wi-Fi credentials, settings | Phone browser to setup AP | `watch_setup.inc` |
| In | SMS | Cellular network | `ext_lte.c` |
| In | Transcript, reply text and audio (C only) | OpenRouter | `voice_chat.c` |
| Out | Battery status, steps, HR | Phone, Gadgetbridge `status`/`act` | `svc.c` |
| Out | Notification dismiss/open, music cmds, call accept/reject/end, find phone | Phone | `svc.c` |
| Out | Battery log dump | MEDIA_CMD notify | `hal_esp32.c` (RAM log) |
| Out | SD listing/file contents | BLE XFR notify | `sd_xfer.c` |
| Out | `/api/status` JSON (battery, uptime, psram, phone, sd, prefs) | LAN client | `watch_setup.inc` |
| Out | Forecast request with lat/lon | Open-Meteo | `weather_service.c` |
| Out | SMS | Cellular network | `ext_lte.c` CMGS |
| Out | Recorded audio and prompts (C only) | OpenRouter | `voice_chat.c` |

## Persistence

- Preferences: `nosprefs` blob mirrored to SD `/sdcard/watch` and NVS (namespace "wos"). SD is authoritative; failed SD saves stay dirty. See [[Storage and persistence]].
- Wi-Fi credentials: `wifi_cred` to SD `/sdcard/watch/wifi` plus NVS mirror, checkpointed 5 s after a successful join and retried while dirty. Stored in plaintext.
- Weather location: NVS "weather_loc" (NVS only).
- Voice: NVS "voice_chat" (key, stt/tts/voice) and "voice_hist" (history, checksummed). NVS only.
- Battery log: 1440 samples at 60 s in RAM only, lost at reboot.
- Weather cache, phone-link state, discovery table, SMS inbox and unread counts: RAM only.

## Interactions

- [[Power and battery]]: radio on/off intent, DFS without light sleep, battery status sent to the phone, battery history ring.
- [[Storage and persistence]]: preferences, Wi-Fi credentials, SD transfer service, `weather_loc` and voice NVS keys.
- [[Engine and scheduling]]: main-loop ordering of `hal_setup_poll`, `radio_preferences`, `ble_xfer_poll`, OTA runtime-policy blocking.
- [[Hardware and drivers]]: ext port, UART0 modem, I2S codecs on C, SD bus.
- [[Health and activity]]: realtime steps/HR pushed to the phone via `act`.
- [[Rendering pipeline]]: Connectivity, Chat, Cellular and Call screens draw through the standard screen interface.

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| high | Weather location cannot be set in NOS; Wi-Fi weather never starts on a fresh install | `weather_service.c`; only caller is legacy `main.c:776` | Weather works only via the phone (Gadgetbridge) or if old firmware stored `weather_loc` |
| high | Unauthenticated HTTP routes (`/notify`, `/cmd`, `/media`, `/cover`); server never stopped after setup | `hal_esp32.c:1812-1831`, `1979` | Anyone on the same LAN can inject notifications or commands while on Wi-Fi |
| high | BLE XFR SD service has no auth or bonding | `ble_xfer.c:73`, `hal_esp32.c:1558` | Any nearby BLE central can read, overwrite and delete SD files outside the protected set |
| medium | No BLE bonding, fixed static address | `hal_esp32.c:1485`, `1558-1560` | Phone link open to any central; trackable constant address |
| medium | Chat is registered on B but non-functional | `registry.c:13`, `voice_chat.c` stub branch, `con_chat.c:131` | Dead app entry on B |
| medium | OTA accepts plain HTTP, no signature check, `/update` shows the token during a session | `ESP_HTTPS_OTA_ALLOW_HTTP=y`, `ota.c url_valid` | A LAN attacker could push firmware during a 10 minute setup session |
| medium | LTE: boot probe powers the modem every boot, `nos_prefs.lte` not applied at boot, 8 s idle auto-off, no SMS notifications, RAM-only unread, no voice or data | `ext_lte.c`, `sys_control.c:234`, `con_cell.c` | Battery cost at boot and no SMS alerts |
| medium | Bluetooth screen has no BLE on/off toggle | `set_connectivity.c:45-54` | BLE can only be toggled from the Control Center |
| low | Battery log tick/dump has no NOS caller and is RAM only | `hal_esp32.c:1270` | Battery history to phone only on subscribe |
| low | `hal_locate_active` (0xF1) is dead in NOS | `main.c:1180` | Phone cannot ring the watch via the TIME char |
| low | `ble_drop_all` unused and assumes handles 0-7 | `hal_esp32.c:1050` | Dead code |
| low | `HAL_FEAT_AI` permanently OFF | `hal_esp32.c:906` | None |
| low | `ota_from_sd`/`ota_from_url` dead | `main.c:693-695` | No SD-card update on NOS |
| low | Wi-Fi air analyzer has no NOS UI | `hal_esp32.c:2067-2243` | Dead code |
| low | Gadgetbridge actfetch/http/calendar/nav/gps unhandled | `svc.c:1190` | No activity history fetch or navigation |
| low | Country hard-coded to IN; Wi-Fi credentials plaintext on SD/NVS | `hal_esp32.c:2291` | Channel limits elsewhere; credential exposure |
| low | No DST handling (fixed tz offset) | `nos/svc.c apply_time` | Clock offset wrong across DST until phone resends time |
