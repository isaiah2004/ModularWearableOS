---
title: "Weather"
id: app-weather
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Weather

Source: `nos/apps/out_weather.c` (952 lines). Read-only source review; nothing built, flashed or run on a board, and no serial access. Evidence is `repo-relative/path:line`.

## Summary

Weather is a three-page full-screen forecast (`scr_weather`, `nos/apps/out_weather.c:907`) and the Home widget `wg_weather` (`out_weather.c:945`). Both draw one shared "view" of data they do not own. The data lives in the services hub: `svc_weather()` returns a RAM struct (`nos/svc.h:88`, `nos/svc.c:337`) filled from two feeds:

- Phone push: a Gadgetbridge `{"t":"weather"}` line (`nos/svc.c:1174` dispatches to `gb_weather`, `svc.c:1073`). It is used for 3 h after the last push (`svc.c:416-421`).
- Wi-Fi: Open-Meteo, fetched by the ESP task `weather` (`firmware/main/weather_service.c:82-142`) and copied into the svc struct every 10 s (`svc.c:412-423`, `svc.c:389-410`). It needs Wi-Fi up, a stored location and a clock.

The app does no network I/O, writes nothing persistent, and reads no NVS or SD path. The one sensor it touches is the optional BME280 strap module, which supplies a single "ON WRIST" temperature line (`out_weather.c:630`, `:674-679`).

Status is partial. The rendering and state machine are complete. The phone path shows today only, no forecast. The Wi-Fi path is inert on a fresh NOS install because nothing in NOS can set its location (Gaps 1).

## Features

- **Now page** (`draw_now`, `out_weather.c:655-680`). Animated dot icon for nine conditions (`WX_*`, `nos/svc.h:67`; drawn by `wx_icon`, `out_weather.c:248-282`). Large temperature in C or F from `nos_prefs.units_metric` (`:519-523`). Condition text, and an H/L line (`:524-529`). Age label such as `PHONE / 12M AGO`, `WI-FI / 2H AGO`, `UPDATING`, `STALE` (`:391-399`). FEELS / HUMID / WIND row, wind in km/h or mph (`:645-653`). Optional `ON WRIST nn` line when the strap is present (`:674-679`).
- **Hourly page** (`draw_hours`, `:682-710`). "NEXT n HOURS" title, then hour label, mini icon and temperature per entry. Six per row.
- **Days page** (`draw_days`, `:712-752`). TODAY or a weekday, mini icon, low, an 8-segment bar on the week's range, high.
- **Icon animation.** One 120-tick loop (`:47`). It plays two loops on entry, on tap, or on a new condition, then holds (`:48`, `:618-619`, `:896`, `:462-463`).
- **Tap.** On page 0 a tap restarts the icon (`:894-897`). Other taps do nothing. There are no buttons.
- **Paging.** Three pages snap vertically on the engine's spring (`msc_*`, `:608`, `:871-893`). A page change fires `FB_TICK` feedback (`:620-624`).
- **Data changes.** A new view crossfades: 4 frames out, 4 in (`:50`, `:449-462`). The temperature morphs when the digit count matches (`hero_temp`, `:544-563`).
- **Empty states** (`:494-515`). "Phone not connected" when neither phone nor Wi-Fi is up. "No weather yet" otherwise. The state line reads UPDATING for the first 30 s after a link exists (`:432`), then NO DATA.
- **Widget** on Home page 4 (`nos/apps/sys_home.c:203`, header `:5`). Same glance layout. A pressed card shows for 0.1 s after touch (`out_weather.c:927`, `:934`). Tap opens the app with no rebuild (`:938-943`, `:606`).
- **Sim-only demo knob.** `NOS_WX_DEMO=<cond>|none` replaces the feed with canned data (`out_weather.c:291-297`, `:298-325`). Compiled out on ESP (`:290`, `:331`).

## Reads

Service getters:
- `svc_weather()` (`svc.h:88`, `svc.c:337`). Read once per view tick through `wx_data()` (`out_weather.c:288-333`, `:423`).
- `svc_sys()` fields `phone_connected` and `wifi_up` (`out_weather.c:425`, `:429`, `:445`). Fields at `nos/svc.h:181-182`, written at `nos/svc.c:995-996` and `:1217-1220`.

HAL:
- `hal_epoch_seconds()`: wall clock for age, "TODAY", and the past-hour filter (`out_weather.c:303`, `:355`, `:386`).
- `hal_ext_present()` with the `EXT_ENV` bit, then `hal_env_get()`: strap temperature, read every app tick (`out_weather.c:630`; `nos/hal_ext.h:57`, `:64`, `:173`). The getter returns a cached sample from `firmware/main/ext/ext_env.c:178-188`.

Preferences:
- `nos_prefs.units_metric` (`out_weather.c:521`, `:562`, `:650-651`).
- `nos_prefs.tz_min`: demo only (`out_weather.c:305`). The phone path reads it when it builds `utc_offset_seconds` (`nos/svc.c:1079`), so phone hour labels use the watch's timezone pref, not the phone's.

Indirect, inside the services that feed the app (not the app itself):
- `hal_weather_get()` (`nos/svc.c:372`, `:392`).
- `hal_get_datetime()` for the night icon choice (`nos/svc.c:338`, `:346`, `:353`).
- `hal_status(HAL_FEAT_WIFI)` (`nos/svc.c:996`; the fetch task checks the same thing at `firmware/main/weather_service.c:93`).

Engine state, not data: `nos_t`, `nos_frame`, `nos_enter_t()`, `nos_revealed()`, `nos_subview`, the blur helpers (`ui_fblur_*`, `nos_mblur_*`).

The app reads no NVS key, no prefs key other than the two above, and no SD path.

## Writes

App-owned RAM only:
- `W` (`out_weather.c:588-599`): frame, scroll, page, drag state, scrollbar fade, strap value, blur state. Cleared on enter (`:601-611`).
- `s_cur`, `s_prev`, `s_have_view`, `s_swap_t`, `s_conn_since`, `s_cond_ver`, `s_view_frame` (`:337-343`). A `static` prepared copy (`:422`). The cloud dot cache `s_cloud` (`:101-102`).
- Widget state `G` (`:910`), `s_from_widget` and `s_widget_frame` (`:585-586`).

Diagnostics, not user data: `diag_action_request/context/result` with `DIAG_DOMAIN_WEATHER` on each change of ws, source, generation, stale or fetching (`out_weather.c:436-444`). The record holds state codes and the age, never text or location (comment `:434-435`). It lands in the RAM diag ring (`nos/diag_log.c:218-228`) and is drained to SD by the `sports_sd` writer (see Background).

Feedback: `nos_feedback(FB_TICK)` on each page change (`out_weather.c:624`).

Sim-only: motion probes (`:633-639`).

The app does not write to svc, prefs, NVS, SD or Gadgetbridge. There is no outbound weather message: `nos/svc.c:1174` is inbound only.

Data-path writes, for the record (not the app's):
- `s_wx` in svc: `gb_weather` (`svc.c:1073-1109`), `wx_from_hal` (`svc.c:389-410`), `wx_merge_forecast` (`svc.c:369-388`), `wx_tick` (`svc.c:412-423`).
- Fetch task: `s_weather`, `s_config`, `s_request` and status strings (`firmware/main/weather_service.c:20`, `:30-41`, `:98-100`, `:126-140`).
- NVS `weather_loc` is written only by `hal_weather_set_location` (`weather_service.c:36`). NOS has no caller (Gaps 1).

## Background behaviour

| Who | What it does | Period, priority, core | Evidence |
|---|---|---|---|
| UI task `main` | `weather_tick` and `weather_draw` run only while the app is the top screen. `wg_tick` runs for the Home widget every home frame. | Core 0 (`firmware/sdkconfig:1707`); priority 1 per the Engine note. Authored clock `NOS_FPS` 40 (`nos/render_config.h:8`), presentation target 60 (`:13`). | `nos/nos.h:73`, `nos/apps/sys_home.c:626-635` |
| `svc_tick` to `wx_tick` | Copies the Wi-Fi cache or refreshes the phone freshness flag into `s_wx`. | 10 s gate. Runs each UI frame when on, and once per second with the screen off (`firmware/main/nos_main.c:2091-2094`, dt=1.0). | `nos/svc.c:412-423`, `:1323` |
| Gadgetbridge line parse | `gb_weather` on `{"t":"weather"}`. | UI task, at most 8 lines per svc tick (`svc.c:49`, `:1236`). Once per second with the screen off. | `svc.c:1174` |
| `weather` ESP task | Open-Meteo HTTPS fetch (current, 12 h, 4 days), parse, atomic store. | `xTaskCreate` 8192 B, prio 2, unpinned (`weather_service.c:175`). Polls every 1 s (`:88`). At least 10 s between attempts (`:94`). Next fetch 1800 s after success, 60 s after failure (`:140`). Needs Wi-Fi STA up, valid location, clock (`:93`, `:96`). | `firmware/main/weather_parse.c` |
| Immediate fetch | `hal_weather_refresh()` bumps the request counter. | From time resync (`firmware/main/hal_esp32.c:1059`) and on got-IP (`:1981`). | |
| BME280 `ext` task | Forced-mode sample every 2000 ms, cached for the app. Also emits a sports sample each time. | Task `ext`, prio 0, 4096 B, SPIRAM, unpinned (`firmware/main/ext/ext_sys.c:186`). Calls `env_service` from its loop (`ext_sys.c:156`). Period `PERIOD_MS` 2000 (`ext_env.c:22`). Sports emit at `ext_env.c:174`. | Runs whenever the strap is present. Not gated on the weather screen. |
| `sports_sd` writer | Drains the diag ring, including weather action records, to SD. | Prio 1, 4096 B, SPIRAM (`nos/sports.c:323`). Calls `diag_log_service` each loop, drains at least every 1 s (`sports.c:282-291`). | `nos/diag_log.c:24`, `:395`, `:463-505` |

Screen off. The app and widget stop ticking, and nothing is drawn. Services still run at 1 Hz (`nos_main.c:2094`). Wi-Fi fetch and phone parsing continue if the radios are on. The strap sensor keeps sampling if present. Its behaviour in the OFF state was not traced. On wake, the first UI frame calls `wx_view_tick`. If the feed changed, that starts the crossfade (`out_weather.c:449-462`).

Closed. `scr_weather` has no leave callback and no keep-awake flag (`out_weather.c:907`, flags 0). Its `weather_tick` stops. Services keep running. The Home widget keeps ticking while Home is shown.

Radio policy. Wi-Fi is off by default (`nos/prefs.c:150`). `radio_preferences` applies `nos_prefs.wifi` (`firmware/main/nos_main.c:953-960`). Wi-Fi power cost is covered in [[Power and battery]].

## Persistence

- The app persists nothing.
- The forecast is RAM only: `s_wx` (`nos/svc.c:69`) and the fetch task's `s_weather` (`firmware/main/weather_service.c:20`). After a reboot the glance is empty until a phone push or a Wi-Fi fetch arrives. No cache survives.
- NVS `weather_loc` (version 1: lat, lon, city[32]). Read at `weather_service.c:163`, written only by `hal_weather_set_location` (`:36`). The NOS build has no caller for the write (Gaps 1). A board that ran legacy firmware with a saved location keeps it, because NVS survives a reflash. That is unverified on hardware.
- SD: diagnostics only, under `/sdcard/watch/diagnostics/diag-NN.bin` (`nos/diag_log.c:24`, `:395`). No text or location is in those records (`out_weather.c:434-435`).
- Nothing about weather goes to the phone.
- Units preference `nos_prefs.units_metric` is saved by the prefs system and only read here.

## Navigation

Opened from:
- Launcher app list: `nos/apps/registry.c:20` ("Weather", DOT_ICON_CLOUD).
- Home widget, page 4 (`nos/apps/sys_home.c:203`, index 2 of `k_wg`). Tap runs `wg_tap` (`out_weather.c:938-943`), which calls `nos_open(&scr_weather)` and sets `s_from_widget` so the glance is not rebuilt (`:606`).
- Split face, third complication row, only on four-row faces: `nos/faces/face_split.c:226` (inside `face_split_hit`, `:218`).

Opens: nothing. The app has no buttons and no links to other screens. Back is the engine gesture.

Input (`weather_input`, `out_weather.c:871-905`):
- Vertical drag and fling page the view. Only a Y-axis drag starts a page drag (`:880-893`).
- A tap on page 0 replays the icon (`:894-897`).
- Input is ignored while an empty state is shown (`:875`).

## Gaps

Ordered by severity. Each has code evidence. Where a doc disagrees with the code, both are cited.

1. **high. Wi-Fi weather has no in-NOS way to set its location.** The only caller of `hal_weather_set_location` is the serial command in `firmware/main/main.c:776` (`location LAT LON CITY`, help at `main.c:892`). `main.c` is not in the NOS source list (`firmware/main/CMakeLists.txt:22-54` lists `nos_main.c`, `${CORE_SRCS}` and `${NOS_SRCS}`). The NOS serial handlers (`firmware/main/nos_main.c:1150-1320`) have no `location` or `weather` command. The only weather reference in `nos_main.c` is a scroll snapshot (`:1069`). Wi-Fi is off by default (`nos/prefs.c:150`). Result: a phone-less watch gets no Wi-Fi weather unless an old `weather_loc` blob survived in NVS. The empty-state hint "Set weather location over USB" (`firmware/main/weather_service.c:20`) points at a path this build does not compile. `docs/os-map/systems/Connectivity and voice.md:19` already notes the missing setter.

2. **medium. The phone's location is ignored.** A BLE time-sync write carries `[epoch:4][tz_min:2][lat:4][lon:4]city` and sets `g_loc_have` (`firmware/main/hal_esp32.c:862-891`). `hal_location` returns it when no stored location exists (`hal_esp32.c:911-918`). `weather_task` reads only `weather_loc` (`weather_service.c:90`, `:163`). A watch that already has the phone's coordinates cannot fetch Wi-Fi weather from them.

3. **medium. The empty-state copy is unreachable.** `empty_body` returns the HAL status string first (`out_weather.c:498`). That string is never empty: the initial value is "Set weather location over USB" (`weather_service.c:20`), and svc copies it every 10 s (`nos/svc.c:398`, `:412-423`). So the pairing hint "Pair in Gadgetbridge as Bangle.js NOS" (`out_weather.c:499`) and the "Your phone sends it" / "It comes over Wi-Fi" lines (`:500`) never show on device. The user is told to use USB instead of pairing.

4. **medium. The phone path has no forecast beyond today.** `gb_weather` zeroes the struct and sets `ndays=1` with no hours (`nos/svc.c:1073-1098`). It deliberately does not merge the Wi-Fi forecast (`svc.c:1102`, "Do not combine forecasts from a separately configured Wi-Fi location"). While phone data is fresh, `wx_tick` returns before the Wi-Fi merge (`svc.c:416-421`). Result: the hourly page shows "NEXT 12 HOURS" and "[ NO HOURLY FORECAST ]" (`out_weather.c:686-691`). The days page shows "1 DAYS" (`:717`). Doc conflict: `docs/PHONE.md:58` says the hourly and daily forecast "comes from the Wi-Fi weather when available". The code does not do that, so the doc is stale.

5. **medium. Phone data vanishes after 3 h instead of going stale.** After 3 h, `wx_tick` falls back to `wx_from_hal` (`svc.c:416-423`). That clears `s_wx` and returns early when Wi-Fi is not valid (`svc.c:389-400`). With no Wi-Fi location the glance drops to an empty state, so the last good data is lost. "STALE" applies only from 1 h (`svc.c:418-419`, label `out_weather.c:393`).

6. **medium. The hourly strip is five points, two hours apart, titled as consecutive hours.** `weather_parse.c:40-50` takes `first + i*2` for `WEATHER_HOURS` entries, and `WEATHER_HOURS` is 5 (`core/weather.h:4`). The span is about 8 h. The request asks for 12 hours (`firmware/main/weather_service.c:107`). The two-row "strip of six" layout (`out_weather.c:3-4`, `:693-698`) cannot appear on device, because five entries give one row. Only the sim demo fills 12 (`:311`). The title says "NEXT 5 HOURS" for data that is not consecutive. On an empty page the title reads "NEXT 12 HOURS" (`:686-687`).

7. **medium. The Wi-Fi path has no humidity, wind, UV or rain chance.** The Open-Meteo request asks only for temperature, apparent temperature and weather code (`weather_service.c:103-107`). `svc.c:405` sets those fields to -1, and the app prints dashes (`out_weather.c:648-652`). The phone path stores `uv`, `rain_pct` and `wind_dir` (`svc.c:1091-1095`), but no code draws them. Only the demo sets them (`out_weather.c:309`).

8. **medium. Nothing survives a reboot.** `s_wx` and `s_weather` are RAM only (`svc.c:69`, `weather_service.c:20`). A reboot with no phone and no Wi-Fi leaves the glance and widget empty.

9. **low. No refresh control in NOS.** `weather_input` has no long-press or tap refresh (`out_weather.c:871-905`). The legacy app had one (`core/apps/app_weather.c:233`), but that file is not built (`firmware/main/CMakeLists.txt:22-54`). Nothing in NOS calls `hal_weather_refresh` except time resync and got-IP (`hal_esp32.c:1059`, `:1981`). The watch cannot ask the phone for weather, and there is no outbound weather message.

10. **low. The "4 s" icon loop depends on frame rate.** `out_weather.c:19` and `:47` say 4 s. The loop counts UI ticks, not time (`out_weather.c:619`, `:916`). With the authored clock at 40 Hz (`nos/render_config.h:8`), 120 ticks is 3 s. At the 60 Hz presentation target (`:13`) it is 2 s. At 30 Hz it is 4 s.

11. **low. The "ON WRIST" label is probably misleading.** `out_weather.c:677` labels the BME280 reading as on-wrist temperature. The header calls it the strap line (`out_weather.c:3`), and the module is an optional add-on (`nos/apps/set_modules.c:77`, `nos/hal_ext.h:21`). If it is worn against the skin, body heat likely raises the reading. This is an inference and was not measured.

12. **Missing features a user would expect from a modern watch.** Sunrise and sunset. Rain chance: `rain_pct` exists but is not drawn or fetched, so there is no "rain soon" view. UV index and wind direction (stored, not drawn). Humidity and wind on the Wi-Fi path. Severe-weather alerts. Air quality. A choice of location on the watch, or use of the phone's location (Gaps 1 and 2). Notifications when rain or a storm is coming. A last-known forecast across reboot (Gap 8).

Dead buttons: none, because the app has no buttons. The missing affordance is that hourly and day rows cannot be tapped.

## Related systems

- [[Services hub]]: `svc_weather`, the Gadgetbridge weather handler, `wx_tick`, and the hub's data sources.
- [[Connectivity and voice]]: Gadgetbridge link, Wi-Fi, the Open-Meteo fetch task.
- [[Hardware and drivers]]: the BME280 strap sensor and the radios.
- [[Engine and scheduling]]: task table, priorities, the frame loop, and screen-off ticking.
- [[Storage and persistence]]: `weather_loc` NVS key and the diagnostics SD path.
- [[Power and battery]]: Wi-Fi cost and screen-off behaviour.
- [[Shell and faces]]: Home pager widget slot and the split-face complication.
- [[Sports and workouts]]: the `sports_sd` writer that drains diagnostics, and the sports journal fed by the BME280 samples.
