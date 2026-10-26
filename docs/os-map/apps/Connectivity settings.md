---
title: "Connectivity settings"
id: sys-connectivity
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Connectivity settings

## Summary

Connectivity settings is the Wi-Fi and Bluetooth settings page. Source: `nos/apps/set_connectivity.c` (286 lines). One static state block (`network_state`, `nos/apps/set_connectivity.c:11-22`) backs four screens: the list `NETWORKS` (title `WI-FI` or `BLUETOOTH`), `WIFI_KEYBOARD`, `PHONE_SETUP` and `BLE_DEVICE`. The list switches between Wi-Fi and Bluetooth mode through `set_connectivity_open(bt)` (`nos/apps/set_connectivity.c:144-152`).

The page owns no radio. Its only persistent state is the Wi-Fi intent `nos_prefs.wifi`. The work is done by HAL calls: Wi-Fi and BLE scans, a queued Wi-Fi join, the phone-setup hotspot, and BLE central connect and disconnect. Radio state comes back as one-line strings that the page polls every frame. The file makes no `svc_*` calls.

Status: **partial**. On the 1.43-B source path, the Wi-Fi flow is complete: scan, password entry, credential save after a verified join, and a 10-minute phone-setup hotspot. The Bluetooth flow is a link-layer connect to one nearby advertiser. The page never shows the Gadgetbridge phone link, and it has no BLE on/off control. This card is source-only. Nothing was run on a watch.

Needs: ESP32-S3 Wi-Fi, which is 2.4 GHz only (`firmware/main/hal_esp32.c:1605` comment), and NimBLE with 2 connections (`firmware/sdkconfig:753`). On the B board, SD is used for the checked settings and credential copies. Without SD the NVS mirror is used (`firmware/main/hal_esp32.c:228-257`). No IMU, RTC or cellular dependency.

Related: [[Settings]] (opens both radio pages), [[Control Center]] (its Wi-Fi and Bluetooth tiles write the same prefs), [[Services hub]], [[Power and battery]], [[Storage and persistence]], [[Hardware and drivers]], [[Connectivity and voice]], [[Shell and faces]], [[Engine and scheduling]].

## Features

### Wi-Fi mode (`bt == 0`, title `WI-FI`, `nos/apps/set_connectivity.c:136`)

The list rows are 84 px, starting at y 100 (`:120`). Row order is fixed: row 0, row 1, the scan rows, then two tail rows.

- **Wi-Fi on / Wi-Fi off** (row 0, `:56`). The title shows the intent `nos_prefs.wifi`, not the radio state. The subtitle is the HAL state string. Tap flips `nos_prefs.wifi`, calls `prefs_save()`, and when turning off also calls `hal_setup_set(0)`, which ends a phone-setup hotspot (`:100-104`).
- **Phone setup** (row 1, `:57`, `:105`). Pushes `PHONE_SETUP`.
- **Network rows** (`:60-63`, up to 32). Title is the SSID. The firmware reports an empty SSID as `(hidden)` (`firmware/main/hal_esp32.c:1654`). Subtitle is `<rssi> dBm / channel <n>`. Tap opens `WIFI_KEYBOARD` with the SSID filled in (`:111-115`).
- **Refresh networks** (tail, `:58`, `:106-110`). Subtitle `2.4 GHz networks only`. Tap sets `scan_at=0`. If Wi-Fi is off, the tap also sets `nos_prefs.wifi=1` and saves (`:107-108`).
- **Other network** (tail+1, `:59`, `:111-115`). Opens the keyboard with an empty SSID for a hidden network.
- **Placeholders** (`:64`): `Scanning...` while Wi-Fi is on, otherwise `Wi-Fi is off`.

### Bluetooth mode (`bt == 1`, title `BLUETOOTH`)

- **Connect your phone** (row 0, `:46`, `:84`). Opens `BLE_DEVICE` in PHONE LINK mode.
- **Device connection** (row 1, `:47`). Subtitle is the HAL BLE state string.
- **Nearby devices** (up to 24, `:50-53`). Title is the advertised name or `Unnamed device`. Subtitle is `<addr> / <rssi> dBm`. Tap selects the device and opens `BLE_DEVICE` (`:93-96`).
- **Disconnect accessory** (tail, `:48`, `:85-88`). Subtitle `Keeps the phone link`. Calls `hal_ble_disconnect_device()`. The subtitle matches the code: only the central (accessory) handle is terminated (`firmware/main/hal_esp32.c:1356-1360`).
- **Refresh nearby BLE** (tail+1, `:49`, `:89-92`). Subtitle `BLE only; no audio profiles`. Calls `scan()`.
- **Placeholder** (`:54`): `Looking for devices` / `Wake the device and enable pairing`. This is also what shows when BLE is off (see Gaps).

### WIFI_KEYBOARD (`nos/apps/set_connectivity.c:154-223`)

- Title `NETWORK NAME` or `PASSWORD`. Three key layers (letters, digits, symbols, plus a second symbol layer) from `keys()` (`:154-160`). Bottom row: SHIFT or ABC, 123 or #+=, SPACE, DEL. Second row: BACK or SHOW/HIDE on the left, NEXT or CONNECT on the right.
- SSID is capped at 32 bytes (`:198`). Password is capped at 63 characters and must be 8-63 characters or empty (`:211`). Empty means an open network. The display shows the last 23 characters, or asterisks while hidden (`:172-176`).
- CONNECT sets `nos_prefs.wifi=1`, saves prefs, logs a diag request, calls `hal_wifi_connect(ssid, pass)`, zeroes the password, and pops back to the list (`:212-218`). The join is asynchronous. The page does not report success. The list state line does.

### PHONE_SETUP (`nos/apps/set_connectivity.c:226-252`)

- Idle: text that says it starts a temporary hotspot which closes after 10 minutes (`:231-235`), and a START SETUP button (hit box x 98-368, y 348-412, `:241`).
- START sets `nos_prefs.wifi=1` and saves (`:242`), then calls `hal_setup_set(1)` (`:244`). Failure shows `Could not start; try again` (`:246`).
- Active: shows the firmware SSID `Watch-Setup-XXXX` and the random password (`:231-235`), plus the instruction `Open http://192.168.4.1`. STOP calls `hal_setup_set(0)` (`:244`).

### BLE_DEVICE (`nos/apps/set_connectivity.c:254-285`)

- PHONE LINK mode (no device selected): static instructions only. Open Gadgetbridge, add device Bangle.js NOS, allow notifications and location, and the phone starts the connection (`:259-263`). No button and no status line.
- Device mode (`N.selected` set): name or `Unnamed BLE device`, address, the HAL status string, `BLE connection only`, `Device features need a matching app`, and a CONNECT button (hit box x 113-353, y 343-407, `:276`). Failure text `Refresh or disconnect current device` (`:280`).

## Reads

**Service getters:** none. The file has zero `svc_` calls. `svc_sys()->phone_connected` and `wifi_up` are set in `nos/svc.c:995-996`, but this page does not read them.

**Preferences:** `nos_prefs.wifi` only (`:36`, `:56`, `:64`, `:101-102`, `:107-108`, `:213`, `:242`). **`nos_prefs.ble` is never read here.** Only Control Center reads it (`nos/apps/sys_control.c:174`).

**HAL calls:**

| Call | Where | Returns |
|---|---|---|
| `hal_wifi_info(N.state, 96)` | `tick`, `:126` | Deferred-stage text, or the last join message (`firmware/main/watch_setup.inc:18-25`) |
| `hal_ble_device_status(N.state, 96)` | `tick` `:125`, `device_tick` `:254`, `device_enter` `:255` | BLE status text (`firmware/main/hal_esp32.c:669-680`) |
| `hal_wifi_scan(N.aps, 32)` | `scan()` `:36` | Cached result of the last completed sweep, not a live scan (`hal_esp32.c:1667-1703`) |
| `hal_ble_scan(N.devs, 24)` | `scan()` `:36` | Entries seen in the last 30 s (`hal_esp32.c:1568-1579`) |
| `hal_setup_info(...)` | `setup_tick` `:226` | Hotspot SSID, password and active flag (`watch_setup.inc:27-31`) |
| `hal_millis()` | `:38`, `:128-129` | 3 s scan throttle |

Other reads: `nos_leaving_covered()` (`:164`, keeps the keyboard draft through alerts). The typed SSID and password are user input. Everything in `N` is RAM only.

Exists but not read here: `hal_phone_connected()` (`firmware/main/hal_esp32.c:1159-1162`). Other screens read it: `nos/apps/sys_notifs.c:348`, `nos/apps/con_music.c:91`, `nos/apps/set_settings.c:417`, `nos/apps/set_modules.c:289`.

Note: the Wi-Fi row label reads intent (`nos_prefs.wifi`), not radio state. The state line can show `Wi-Fi deferred` while the label says `Wi-Fi on`.

## Writes

- **Prefs:** `nos_prefs.wifi` is set by the row toggle (`:102`), Refresh networks (`:108`), keyboard connect (`:213`) and START SETUP (`:242`). Each write is followed by `prefs_save()`, which marks RAM dirty. The checkpoint runs later (see Persistence). `nos_prefs.ble` is not written here.
- **Radio requests:** `hal_setup_set(0)` when Wi-Fi turns off (`:102`) and on STOP (`:244`). `hal_setup_set(1)` on START (`:244`). `hal_wifi_connect(ssid, pass)` (`:215`, queued at `firmware/main/hal_esp32.c:2057-2064`). `hal_ble_disconnect_device()` (`:87`). `hal_ble_connect_device(addr)` (`:278`).
- **Scan side effects:** `hal_ble_scan` renews the BLE discovery lease (`firmware/main/hal_esp32.c:1571`). `hal_wifi_scan` may start an async sweep (`hal_esp32.c:1676-1686`).
- **Radio power is not set here.** The Wi-Fi and BLE intents are applied by `radio_preferences()` in the frame loop (`firmware/main/nos_main.c:953-961`, called every loop pass at `:1987`).
- **Diagnostics:** every radio action logs a `DIAG_ACTION_REQUEST`, `CONTEXT` and `RESULT` with numeric command IDs `0x6101`-`0x6106` and the name hash of `NETWORKS` (`:74-79`; call sites `:86`, `:90`, `:101`, `:107`, `:214`, `:243`, `:277`). The records carry no SSID, address, device name or password (comment at `:73`). The diag service drains the ring to SD `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`).
- **RAM state:** `N.*` (`:12-21`), including the typed SSID and password, the selected device and the list. The password is zeroed on connect (`:217`) and on keyboard leave unless an alert covers the screen (`:164-165`).

## Background behaviour

**The page itself:** no task and no timer. `tick()` and `draw()` run only while it is the top screen. The engine calls `tick_screens` once per frame (`nos/nos.c:2101`). The nominal animation cadence is 40 Hz (`nos/nos.h:4-5`, `nos/render_config.h:8`), and presentation can go up to 60 Hz (`nos/render_config.h:13`). Each tick reads two HAL status strings.

**BLE discovery:**
- Runs only while the list is in Bluetooth mode and on top. `tick()` calls `scan()` at most every 3 s, and only while the list is not dragged or pressed (`:123-131`).
- Each `hal_ble_scan()` renews a 5 s lease (`firmware/main/hal_esp32.c:611`, `:1571`). Scan window is 10 ms in every 160 ms (`:620-621`).
- After the page closes, nothing renews the lease, so new discovery stops within 5 s. A discovery that has already started runs its full 8 s (`:622`). The radio tail is therefore up to about 8 s.

**Wi-Fi scanning:**
- No background sweep. A sweep starts only when some screen calls `hal_wifi_scan()`. Callers: this page (`:36`), the serial `network scan` command (`firmware/main/nos_main.c:1551`), and `core/apps/app_wifi.c:51` and `core/apps/app_netmon.c:75`. The last two are not confirmed to be in the NOS build.
- A new sweep starts only if none is busy, the last attempt was more than 1.2 s ago, and the last completed sweep was more than 8 s ago (`firmware/main/hal_esp32.c:1672-1673`). A 9 s watchdog stops a stuck sweep (`:1694`).

**Wi-Fi radio (runs once enabled, independent of this page):**
- `radio_preferences()` applies `nos_prefs.wifi` through `hal_wifi_set_enabled()` on each loop pass (`firmware/main/nos_main.c:956-961`). It does nothing while prefs are loading (`ready = !prefs_loading()`, `:958`). The radio stays off while an SD read is unresolved.
- The main loop runs in both the awake and screen-off paths. The screen-off branch drops the CPU to 80 MHz and runs a 1 s background service tick (`firmware/main/nos_main.c:2091-2094`). The radio apply is in the shared block at `:1984-1987`, so it also runs with the screen off. The deep-sleep path was not traced on a device.
- The `wifi-settings` worker (`xTaskCreate`, 6144-byte stack, priority 2, unpinned; `firmware/main/hal_esp32.c:2268`; loop `:2007-2055`) handles the credential queue, the credential save, and `air_supervise()`.
- The station reconnects with exponential backoff: 2 s, doubling to a 300 s cap (`firmware/main/hal_esp32.c:1865-1876`). The backoff resets on GOT_IP (`:1974`).
- On GOT_IP the watch starts its HTTP server (`web_start()`, `:1979`, defined at `:1798`), starts NTP (`:1980`), and refreshes weather (`:1981`). These run whenever Wi-Fi joins, not only during phone setup. See Gaps.

**Phone-setup hotspot:**
- APSTA mode, WPA2-PSK, channel 1, 2 clients (`firmware/main/hal_esp32.c:1912-1919`). The password is random on each start (`:1913-1914`).
- A one-shot 600 s timer `wifi_ap` (`:1924`) calls `ap_stop_cb()`, which calls `hal_setup_set(0)` (`:1880-1883`).
- `PHONE_SETUP` has no leave hook (`set_connectivity.c:252`). The hotspot keeps running after the user leaves the page, until STOP, Wi-Fi off, or the 10-minute timer. `SCR_KEEPAWAKE` holds the display on only while the page is on top (`set_connectivity.c:252`; `firmware/main/nos_main.c:2096`).
- Portal routes `/`, `/wifi` and `/api/*` return 403 unless the hotspot session is active (`firmware/main/watch_setup.inc:62-64`). OTA routes also need the hotspot session token (`firmware/main/ota.c:143-146`, `:459`). The exception is `/cover`, `/media`, `/cmd` and `/notify` (see Gaps).

**BLE link (runs once enabled, independent of this page):**
- NimBLE host task `nimble_host`, priority `configMAX_PRIORITIES-4`, 4096-byte stack, pinned to core 0 (`firmware/main/hal_esp32.c:1531-1534`; `firmware/sdkconfig:709-712`). A 100 ms NimBLE callout runs `ble_control_tick` (`firmware/main/hal_esp32.c:1368-1374`, `:1551`).
- Advertises as `Bangle.js NOS` (`firmware/main/hal_esp32.c:57`, `adv_start` at `:1312-1331`) when `nos_prefs.ble` is set (`firmware/main/nos_main.c:958-959`). Gadgetbridge connects to it. Phone connections are tracked by `peri_gap_event` (`firmware/main/hal_esp32.c:1376-1420`). Maximum 2 connections (`firmware/sdkconfig:753`).
- Accessory central connect: one attempt with a 10 s timeout (`firmware/main/hal_esp32.c:1362`). On disconnect it only restarts scanning (`:643-645`). There is no reconnect, no GATT discovery, and no bonding (`:1558-1560`).

## Persistence

- **`nos_prefs.wifi`, `nos_prefs.ble` and all other prefs:** RAM copy in `nos_prefs`. `prefs_tick()` flushes when dirty for more than 2 s, or 10 s after the first dirty write (`nos/prefs.c:366-383`, condition at `:380-381`). The flush calls `hal_store_save_checked("nosprefs")` (`nos/prefs.c:349`). On the B board that writes the alternating checksummed slots `/sdcard/watch/settings.0.bin` and `settings.1.bin` (`nos/prefs_disk.c:30-33`; directory `firmware/main/hal_esp32.c:243`), and mirrors to NVS key `nosprefs` (`firmware/main/hal_esp32.c:252`). A failed SD save keeps the RAM copy dirty and retries (`nos/prefs.c:349-353`). The flush runs in both awake and screen-off loops (`firmware/main/nos_main.c:2098`, `:2110`, `:2164`; `nos/nos.c:2349`).
- **Wi-Fi credentials:** one slot. Written after association is confirmed (`firmware/main/hal_esp32.c:2043-2052`) to `/sdcard/watch/wifi/settings.0.bin` and `.1.bin` (`hal_esp32.c:243-246`), and mirrored to NVS key `wifi_cred` (`:252`). Failed attempts are never saved, because association never succeeds. Loaded at boot (`:2297`) and auto-joined (`:2305`). A later successful join overwrites the single slot.
- **Wi-Fi driver config:** RAM only (`firmware/main/hal_esp32.c:2286`, `WIFI_STORAGE_RAM`). The SDK keeps no copy of its own.
- **Phone-setup password:** random per start, RAM only (`:1913-1914`), shown on the watch. The serial `network setup` command also prints it over USB (`firmware/main/nos_main.c:1541-1548`).
- **Portal settings POST** (brightness, auto_bright, dnd, aod): queued by `web_settings` (`firmware/main/watch_setup.inc:146-158`, write at `:158`), then applied and saved by `hal_setup_poll()` (`watch_setup.inc:33-46`). This needs the hotspot session.
- **BLE accessory list, RSSI table and selected device:** RAM only (`firmware/main/hal_esp32.c:547-589`; `nos/apps/set_connectivity.c:12-21`). No bonds: `sm_bonding=0` (`firmware/main/hal_esp32.c:1558`). The selected accessory is forgotten on reboot.
- **Diagnostics:** bounded ring of integer arguments (`nos/diag_log.h:52-54`; `nos/diag_log.c:24`). No secrets.
- No NVS key and no SD file belong to this page's own UI state.
- On a board without SD (`#if !BOARD_HAS_SD`), `hal_store_save_checked` writes NVS only (`firmware/main/hal_esp32.c:228-236`). This is not the target board.

## Navigation

**Opened from:**
- Settings: rows `I_BT` and `I_WIFI` call `set_settings_open_radio()` (`nos/apps/set_settings.c:1851`; wrapper `:2266-2269`), which calls `set_connectivity_open()`.
- Modules: rows `M_WIFI` and `M_BLE` (`nos/apps/set_modules.c:719`).
- Serial/debug: `network wifi`, `network ble`, `network status`, `network setup`, `network scan` (`firmware/main/nos_main.c:1533-1555`). Simulator: `network` (`sim/nos_sim.c:976-979`).
- Not opened by Control Center. Its Wi-Fi and Bluetooth tiles toggle the same prefs (`nos/apps/sys_control.c:173-174`, `:228-229`) and never show this page.

**Opens:**
- `NETWORKS` to `PHONE_SETUP` (`:105`), `WIFI_KEYBOARD` (`:115`), and `BLE_DEVICE` (`:84`, `:96`).
- `WIFI_KEYBOARD` pops back to `NETWORKS` on BACK (`:208`) or CONNECT (`:218`).
- `PHONE_SETUP` and `BLE_DEVICE` have no BACK control in this file. Leaving them depends on the engine's generic gesture, which this card did not trace.
- Taps in the header band (`y < 96`) are swallowed on `NETWORKS` (`:140`).

**Screen flags:** `NETWORKS` has none (`:143`), so the display can sleep while the list is open. `WIFI_KEYBOARD` uses `SCR_HDRAG|SCR_KEEPAWAKE` (`:224`). `PHONE_SETUP` uses `SCR_KEEPAWAKE` (`:252`).

## Gaps

Severity is a judgement from the source. None of these was reproduced on a watch.

1. **high: unauthenticated write endpoints on the station web server.** `firmware/main/hal_esp32.c:1813-1823` registers `/cover` (POST), `/media` (POST), `/cmd` (GET) and `/notify` (POST). The handlers `web_cover` (`:1737`), `web_media` (`:1747`), `web_cmd` (`:1764`) and `web_notify` (`:1771`) do no session check. `setup_authorized()` protects only `/`, `/wifi` and `/api/*` (`firmware/main/watch_setup.inc:62-64`, `:91`, `:100`, `:113`, `:125`, `:146`). `web_start()` runs on every GOT_IP (`hal_esp32.c:1979`). Impact: any host on the same LAN can post notifications (`web_notify` to `hal_notification_post`, `:1771-1795`, `:409`), replace the 125000-byte cover image (`:1716-1744`), tie up the cover writer, inject now-playing text (`web_media` to `media_apply`, `:1747-1762`), and read transport counters.
2. **medium: the Bluetooth page reports "No accessory connected" while the phone is linked.** `hal_ble_device_status` (`firmware/main/hal_esp32.c:669-680`) reads only the central (accessory) state. The phone link is `g_med_conn` and `g_nus_sub` (`hal_esp32.c:1159-1162`, `:1392`, `:1417-1419`). Other screens read it through `hal_phone_connected()` (`sys_notifs.c:348`, `con_music.c:91`, `set_settings.c:417`, `set_modules.c:289`). The PHONE LINK page (`set_connectivity.c:256-263`) is static text with no status and no retry. A user with a working Gadgetbridge link sees the wrong status and cannot check or reset the link here.
3. **medium: no Bluetooth on/off control or state here.** `set_connectivity.c` never reads `nos_prefs.ble`. Only Control Center toggles it (`nos/apps/sys_control.c:174`, `:229`). With BLE off, the list still shows `Looking for devices` (`:54`), and `Connect your phone` still opens the instructions. The only hint is the row-1 text `Bluetooth off` (`firmware/main/hal_esp32.c:671`).
4. **medium: phone setup leaves Wi-Fi on for good.** `setup_input` sets `nos_prefs.wifi=1` and saves (`:242`). STOP only calls `hal_setup_set(0)` (`:244`), which switches the radio back to station mode (`hal_esp32.c:1885-1895`) and does not clear the intent. `nos/prefs.c:150` says Wi-Fi is off by default because it costs 6-90 mA. After one setup session, the radio keeps running until the user goes back and toggles it.
5. **medium: "Refresh networks" is not a refresh, and it silently turns Wi-Fi on.** The row sets `scan_at=0` (`:106-110`). `hal_wifi_scan()` returns the cached sweep and starts a new one only when the last completed sweep is more than 8 s old (`firmware/main/hal_esp32.c:1672-1673`). A tap right after a sweep shows the same list. With Wi-Fi off, the tap also sets and saves `nos_prefs.wifi=1` (`:107-108`), with no message.
6. **medium: the joined network is not named.** The connected status is `Connected / <ip>` only (`firmware/main/hal_esp32.c:1975`). The page shows that line but not the SSID, and the list does not mark the joined network.
7. **medium: scan results carry no security, saved-network or sort information.** `hal_wifi_ap` holds only ssid, rssi and chan (`core/hal.h:95-99`). `wifi_scan_collect` de-duplicates by SSID and keeps SDK order (`firmware/main/hal_esp32.c:1636-1665`). The keyboard asks for a password on every network (`:177`), so an open network needs an empty password typed by hand. Rows show raw dBm values, not signal bars.
8. **medium: one saved Wi-Fi network, and no way to forget it.** `wifi_cred` is a single key (`firmware/main/hal_esp32.c:2004`, `:2297`). Each successful join overwrites it. There is no known-networks list, no forget control, and no priority or auto-join setting.
9. **medium: accessory connections do not persist or reconnect.** Bonding is off (`firmware/main/hal_esp32.c:1558`). `central_event` handles only CONNECT and DISCONNECT (`:627-647`). There is no GATT discovery and no reconnect after a drop. The page offers no forget and no auto-reconnect. `BLE_DEVICE` says `Device features need a matching app` (`set_connectivity.c:269`) but never names those features.
10. **low: raw protocol text on user screens.** The Bluetooth state shows `Not connected (code %d)` with the raw NimBLE reason (`firmware/main/hal_esp32.c:678`; the reason is stored at `:643`). Wi-Fi deferred states show `stage`, `error` and `retries` numbers (`firmware/main/watch_setup.inc:21-23`).
11. **low: Wi-Fi regulatory domain is fixed.** Country code `IN`, channels 1-13, manual policy (`firmware/main/hal_esp32.c:2289-2292`). The user cannot change it, and it sets which channels are scanned and used wherever the watch is.
12. **low: diagnostics are mislabelled and never completed.** `radio_action()` always logs source `NETWORKS` (`set_connectivity.c:77`), including from `PHONE_SETUP`, `WIFI_KEYBOARD` and `BLE_DEVICE`. The Wi-Fi toggle logs `DIAG_RESULT_SUCCESS` before the radio change is applied (`:102-103`, comment `RAM policy`), but `nos/diag_log.h:52-54` says a result records actual completion. The `PENDING` results at `:87`, `:91`, `:109` and `:216` never get a completion record in this file.
13. **low: start can fail silently.** `hal_setup_set(1)` returns 0 while the Wi-Fi air analyzer holds the radio (`firmware/main/hal_esp32.c:1898`). The page shows only `Could not start; try again` (`set_connectivity.c:246`), with no reason.
14. **low: the hotspot has no indicator outside its own page.** It runs up to 10 minutes after the user leaves (`set_connectivity.c:252`; `hal_esp32.c:1924`). Control Center's Wi-Fi tile reads only `nos_prefs.wifi` (`nos/apps/sys_control.c:173`), so nothing outside the page shows the hotspot is up.
15. **low: hard-coded address text.** `Open http://192.168.4.1` (`set_connectivity.c:234`) is a literal. Nothing in the repo sets the softAP address. Not verified on a device.
16. **low: expected features missing.** No airplane mode (no matches in `nos`, `core` or `firmware/main`). No BLE toggle on this page. No connection history or data use. The page raises no alert when a link drops; the state line changes only while the page is open.
17. **sim only: placeholder data.** `sim/nos_hal_sim.c:289-300` returns seven fake APs. `:303-315` returns six fake BLE devices. `:622-626` returns `Watch-Setup-SIM` / `preview12345`. `:628` and `:631` return simulator text. None of this reaches the device.

Checked and not found: dead buttons (every button has a live handler, and PHONE LINK has none by design). Stale device-page fields (the status text refreshes every frame). Wi-Fi list age is covered in item 5.
