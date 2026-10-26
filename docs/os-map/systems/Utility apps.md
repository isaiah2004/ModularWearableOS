---
title: "Utility apps"
tags: [nos, os-map]
updated: 2026-10-23
---

# Utility apps

## Summary

The utility apps are the small tools in the NOS launcher: Alarms, Timer, Stopwatch, World clock, Weather, Compass, Barometer, IMU, Navigate, Music, Find phone, Torch and Recorder. `core/ephem.c` also belongs here, but it is not an app. It is the planet-position maths, and only the Orrery face calls it (`nos/faces/face_orrery.c`).

Most of these screens are thin views. The behaviour that has to keep working with the screen off and the app closed lives in `nos/svc.c` (alarms, timer, stopwatch, music, find, weather mirror), `nos/svc_track.c` (GPS track), `nos/apps/con_memo.c` (the recorder service half) and `nos/imu_rec.c`. All of it is driven from `svc_tick()`. That is the `nos_bg_tick` hook set in `nos/apps/registry.c:62-71`. `nos_main.c` calls it every frame when the screen is on and at 1 Hz in PWR_OFF/AOD (`firmware/main/nos_main.c:2091-2123`).

There is no deep sleep, so alarms, timers and the stopwatch fire with the screen off. Ring screens (`scr_alarm_ring`, `scr_timer_ring`, `scr_find_watch`, `scr_call`) are pushed by the service through `ring_screen()` (`nos/svc.c:659`).

The strap is separate hardware: GY-271 magnetometer, BME280 barometer, u-blox GPS, MAX98357A speaker, INMP441 mic, ERM motor. Its drivers run in the single "ext" task (`firmware/main/ext/`). The strap-dependent apps are Compass, Barometer, Navigate, Recorder and the Music/Find ring sounds. They draw an "absent" page when the presence bit is clear. The bits are set once at boot and never cleared, so unplugging the strap while running is not detected.

Persistence is uneven. Alarms, world-clock favourites and the weather location are saved. The timer, stopwatch and laps are RAM only and are lost on reboot.

## Components

### Alarms (app-alarm; svc-alarm)
- Purpose: up to 8 alarms with repeat days; ring screen with slide-to-stop and a 9 min snooze. The editor works on a draft and commits with generation checks.
- Where it runs: the screens run in the UI task. The firing logic is in `svc_tick` (`nos/svc.c:711-853`). The ring is pushed by `ring_screen`/`ringer_tick` (`nos/svc.c:659,688`), capped at `RING_MAX_MS` 60000.
- Cadence: per frame with the screen on, 1 Hz with it off. The ring wakes the display through `SCR_KEEPAWAKE`. A swipe-away is a 2.5 s grace pop.
- Reads: RTC time, the alarm table, `nos_prefs`. Phone alarms arrive over BLE as `gb_alarms` (`nos/svc.c:1112`).
- Writes: `hal_store_save_checked("nosalarm", alarm_saved)` (`nos/svc.c:728`). Ring output goes to the haptic and speaker through `time_ring`.
- Files: `nos/apps/time_alarm.c:584,903,1093`; `nos/svc.c:706-853`.
- Reality check: the fallback table `AL[]` in `time_alarm.c` (the `a_svc` path) is dead code. The service is always present in the NOS build.

### Timer (app-timer; wg-timer)
- Purpose: countdown with a drum picker, a ring screen and a home widget `wg_timer`.
- Where it runs: the state machine is in `nos/svc.c:855-910` and ticks from `svc_tick`, so it ends with the app closed.
- Cadence: per frame, 1 Hz screen-off. The ring uses `nos_keep_awake` (`nos/apps/time_timer.c:593`).
- Reads: `hal_millis`. Writes: the ring sound and haptic only. There is no `hal_store` write, so a reboot loses a running timer.
- Files: `nos/apps/time_timer.c:781,863,987,1212`. The fallback `TL` (`:334-398`) is dead.
- Shared helpers `tdrum_*`, `time_ring` and `time_pulse_draw` are also used by Alarms and Stopwatch.

### Stopwatch (app-stopwatch)
- Purpose: stopwatch with up to `SVC_LAPS` (30) laps.
- Where it runs: `nos/svc.c:912-938`; `time_sw()`/`time_sw_cmd()` are the app-side accessors. It keeps running with the screen closed because it is a millisecond base held by the service.
- Persistence: none (RAM). Files: `nos/apps/time_stopwatch.c:684`. The fallback `SL` is dead.

### World clock (app-worldclock)
- Purpose: favourite cities with DST rules. Pages are `scr_worldclock`, `scr_wc_pick` and `scr_wc_detail`.
- Reads: `hal_epoch_seconds`, `nos_prefs.tz_min`. A table of 13 cities, each with DST rules, is compiled in.
- Writes: `hal_store_save("nos_wc", &F)` with a magic byte 0x57, count n and idx[4] (`nos/apps/time_worldclock.c:52-60`). This is the unchecked save variant.
- Files: `nos/apps/time_worldclock.c:280,362,437`. The header comment is stale.
- Runs only while open.

### Weather (app-weather; svc-weather; wg-weather)
- Purpose: current conditions, 5 hourly slots and 4 daily slots (`core/weather.h:4-5`).
- Where it runs: the app is a view (`wx_view_tick`, `wx_prepare`, `wx_age_label`). Data comes from the FreeRTOS task "weather" (priority 2, 8192 B internal stack, `firmware/main/weather_service.c:175`). It wakes every 1 s.
- Fetch policy: it requires a valid location, Wi-Fi UP and a valid epoch (`:93-96`). It re-fetches every 1800 s on success and 60 s on failure, with a minimum 10 s gap between attempts (`:140`). The URL is Open-Meteo with `forecast_hours=12` and `forecast_days=4`; `weather_parse.c` keeps what fits the structs.
- A second path is the phone: `gb_weather` (`nos/svc.c:1073`) mirrors Gadgetbridge weather into the same view (`nos/svc.c:337-424`).
- Reads: Wi-Fi state, epoch. Writes: `hal_store_save_checked("weather_loc", weather_config)` (`:36`).
- Files: `nos/apps/out_weather.c:907,945`; `firmware/main/weather_service.c`; `firmware/main/weather_parse.c`.
- Reality check: the only caller of `hal_weather_set_location` is legacy `firmware/main/main.c:776`, which is not built. The NOS build has no way to set a city, so the status text "Set weather location over USB" is the normal state unless a location was stored by an older firmware.

### Compass (app-compass)
- Purpose: heading from the GY-271 magnetometer with calibration and declination.
- Reads: `hal_ext_present() & EXT_MAG`, mag samples from `ext_mag.c`. Writes: calibration offsets (in the mag driver).
- Runs only while open. The "ext" task samples the sensor on demand, with a wake interval of at most 50 ms.
- Absent: an empty state page via `ui_absent`.
- Files: `nos/apps/out_compass.c:733`; `firmware/main/ext/ext_mag.c`.
- Unverified: on first open after boot `M->cur.calibrated` is 0 until the first compute, which may flash a calibration prompt. PLAUSIBLE only.

### Barometer (app-baro)
- Purpose: pressure and altitude from the BME280, with a sea-level calibration page `scr_baro_cal`.
- Reads: `ext_env.c`. Writes: the sea-level reference through `hal_store`. Runs while open. Absent: empty state.
- Files: `nos/apps/out_baro.c:557,716`; `firmware/main/ext/ext_env.c`.

### IMU (app-imu)
- Purpose: live accelerometer/gyro view plus a CSV recorder.
- Where it runs: the screen is the UI task. Recording belongs to `nos/imu_rec.c` and `firmware/main/imu_hal.c` (50 Hz sampler task). Output is an SD CSV plus resumable checksummed intent checkpoints.
- Needs the SD card. With no card the recorder cannot start.
- Files: `nos/apps/out_imu.c:195`; `nos/imu_rec.c`; `firmware/main/imu_hal.c`.

### Navigate (app-nav; svc-track)
- Purpose: GPS position, track recording and GPX export.
- Where it runs: the screen is a view; `svc_track_tick` is called from `svc_tick` (`nos/svc.c:1306`), so a track keeps recording with Navigate closed and the screen off. On stop, the task "track_save" (priority 1) writes the GPX to the SD card.
- Reads: the u-blox GPS through the ext layer and `hal_location` (`firmware/main/hal_esp32.c:911`). Writes: the GPX file on SD.
- Absent: with no strap GPS, the screen shows its empty state and no track can be started.
- Files: `nos/apps/out_nav.c:1111`; `nos/svc_track.c`; `nos/svc_track.h`.

### Music (app-music; svc-music; wg-music)
- Purpose: remote control of the phone's player, with title, state and transport. Data arrives over BLE as `musicinfo` and `musicstate`. Commands go out through the media-control path (`nos/svc.c:276-334`).
- Runs with the app closed because the service holds the state. Music reconnect and its glyph were in the latest commits (3941ba4).
- Writes: nothing persisted. Does not need the strap.
- Files: `nos/apps/con_music.c:793,969`.

### Find phone / Find watch (app-find; sys-find-watch)
- Purpose: Find phone sends `{"t":"findPhone","n":true}` over BLE when connected. Otherwise it calls `hal_find_phone()` (media-notify code 7, `firmware/main/hal_esp32.c:1239`). Find watch is the reverse direction: the phone asks and the service pushes `scr_find_watch`.
- Where it runs: `svc_find_phone` and `call_find_tick` (`nos/svc.c:965-983`). Find watch auto-stops after `2*RING_MAX_MS` (120 s) (`:981`). `find_leave` stops a running phone search (`nos/apps/con_find.c`), so Find phone does not continue after leaving the screen.
- Find watch forces brightness 255 and plays the sfx and haptic. With no strap speaker or motor, it is a silent bright screen.
- Files: `nos/apps/con_find.c:364,492`; `nos/svc.c:965-983`.

### Torch (app-torch)
- Purpose: white or dim red light, using a brightness override (`nos_brightness_override`, `nos/nos.c:576`). Brightness is restored on leave.
- Sets `nos_prefs.torch` to 1 on enter and 0 on leave (`nos/apps/con_torch.c:65,83`). It calls `nos_keep_awake` every tick, so the screen never sleeps while it is open.
- No hardware besides the AMOLED. Files: `nos/apps/con_torch.c:217`.

### Recorder (app-memo; svc-memo)
- Purpose: voice memos, a noise meter (INMP441), and playback (MAX98357A). A three-page pager.
- Where it runs: recording belongs to the HAL recorder in the "ext" task plus a file task (`firmware/main/ext/ext_audio.c:417-455`). `svc_memo_tick` runs from `svc_tick` (`nos/svc.c:1307`), so a recording continues with the screen closed. Completion is detected there and announced with a "SAVED m:ss" toast (`nos/apps/con_memo.c:509-536`).
- Reads: the mic level and wave (`hal_mic_db`, `hal_mic_wave`), `hal_ext_present() & EXT_MIC`.
- Writes: WAV files at `/sdcard/watch/memos/M_YYYYMMDD_HHMMSS_NNNN.wav` (`con_memo.c:47,438-446`). The list is re-scanned by the task "memo_scan" (priority 1, 4096 B) under the SD asset lease.
- Absent: no mic gives an empty state. Starting is rejected if `nos_runtime_recording_allowed()` is false or the file cannot be opened.
- Files: `nos/apps/con_memo.c:1104`; `firmware/main/ext/ext_audio.c`.

### Ephemeris (eng-ephem)
- Purpose: Keplerian planet positions (`ep_helio`, `ep_sky_at`, `ep_orbit`) in soft-float doubles.
- Used only by `nos/faces/face_orrery.c`. No utility app calls it. It is pure maths with no state and no hardware.
- Files: `core/ephem.c`, `core/ephem.h`.

## How it runs

- UI task: the "main" task on core 0, priority 1, boosted per frame. All screens, `svc_tick` and the ring pushes run here.
- Service bus: `svc_tick` calls `svc_track_tick`, `svc_memo_tick`, `svc_cell_tick`, then phone, alarm, timer and stopwatch logic (`nos/svc.c:1303-1329`). Every frame while the screen is on; 1 Hz when it is off (`firmware/main/nos_main.c:2091-2123`).
- Worker tasks: "weather" (priority 2, 8 KB, 1 s loop), "track_save" (priority 1), "memo_scan" (priority 1, 4 KB, one shot), the 50 Hz IMU sampler, and the "ext" task (priority 0, 4 KB PSRAM stack, wake at most 50 ms) for the strap.
- When the screen is off: alarm, timer, stopwatch, music state, find, weather polling, GPS track and recording all continue. Mag, baro and IMU views stop because they only sample while open.
- Ring flow: the service sets a state flag, `ringer_tick` pushes the ring screen, `SCR_KEEPAWAKE` lights the display, and the ring ends by user action or at `RING_MAX_MS`.

## Data in / data out

| Direction | Item | Source / sink | Mechanism |
|---|---|---|---|
| in | Alarm table, time | `nos/svc.c` alarm table, RTC | direct calls `svc_alarm_*` |
| in | Phone alarms | Gadgetbridge BLE `alarm` line | `gb_alarms` |
| in | Phone weather | BLE `weather` line | `gb_weather` |
| in | Phone music | BLE `musicinfo`, `musicstate` | `nos/svc.c:276-334` |
| in | Find-watch request | BLE `find` line | `s_find_watch` flag |
| in | Open-Meteo JSON | HTTPS | task "weather" |
| in | Mag, baro, GPS samples | strap I2C/UART | "ext" task getters |
| in | Mic level and wave | INMP441 | `hal_mic_db`, `hal_mic_wave` |
| out | Ring sound and haptic | MAX98357A, ERM motor | `time_ring`, `hal_sfx`, `hal_haptic` |
| out | Find-phone command | BLE | `txf` JSON or `hal_find_phone` code 7 |
| out | Brightness override | display | `nos_brightness_override` |
| out | GPX track | SD | task "track_save" |
| out | Memo WAV | SD `/sdcard/watch/memos/` | ext file task |
| out | IMU CSV | SD | `nos/imu_rec.c` |

## Persistence

| Key / path | Writer | Notes |
|---|---|---|
| `hal_store` "nosalarm" | `svc_alarm_save` (`nos/svc.c:728`) | checked save; version 1, up to 8 alarms |
| `hal_store` "nos_wc" | `fav_save` (`time_worldclock.c:60`) | unchecked save; magic 0x57 |
| `hal_store` "weather_loc" | `hal_weather_set_location` | checked; no NOS caller can set it |
| SD `/sdcard/watch/memos/*.wav` | Recorder | needs the SD card |
| SD GPX tracks | "track_save" | needs the SD card |
| SD IMU CSV and checkpoints | `imu_rec.c` | needs the SD card |
| none | Timer, stopwatch, laps, weather data, torch | RAM only; lost on reboot |

Per project policy, settings live on SD (`/watch/settings.0.bin` and `.1.bin`) with NVS as fallback. Whether each `hal_store` key goes to NVS or SD was not verified here.

## Interactions

- [[Services hub]]: the alarm, timer, stopwatch, music, find, weather and call services are in `nos/svc.c`.
- [[Engine and scheduling]]: the frame loop, `nos_bg_tick`, the 1 Hz screen-off tick and the ring-screen push.
- [[Hardware and drivers]]: strap drivers (mag, baro, GPS, audio, haptic) and the "ext" task.
- [[Storage and persistence]]: `hal_store` keys, SD memo/GPX/IMU paths, the SD lease.
- [[Connectivity and voice]]: Gadgetbridge lines, Open-Meteo over Wi-Fi; Chat and Recorder share the audio path.
- [[Power and battery]]: screen-off 1 Hz service tick, Torch brightness, `nos_keep_awake`.
- [[Shell and faces]]: the Orrery face uses `core/ephem.c`; home widgets `wg_timer`, `wg_weather`, `wg_music`.
- [[Health and activity]] and [[Sports and workouts]]: the Recorder emits `sports_emit(SP_ROUTINE)` on save; the IMU recorder shares the IMU sampler.
- [[Settings and developer]]: `nos_prefs.tz_min`, the torch flag.
- [[Rendering pipeline]]: dot-matrix UI.
- See also [[Gaps]], [[OS overview]], [[Calorie trace]].

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| high | Timer and stopwatch are not persisted. | `nos/svc.c:855-938` (no store call) | A reboot or crash loses a running timer and laps. |
| high | No way to set the weather location in the NOS build. | `firmware/main/weather_service.c:30`; the only caller is the dead `firmware/main/main.c:776` | Weather stays on "Set weather location over USB" unless an old value is stored. |
| medium | Strap presence is latched at boot. | `firmware/main/ext/ext_sys.c`, `ext_priv.h` | An unplugged strap is not detected; the apps show stale or empty data. |
| medium | The ring is silent without the strap. | `nos/apps/time_timer.c`, `nos/apps/con_find.c` (`fw_restore`) | Alarms and find-watch only light the screen. |
| medium | Recorder, Navigate and IMU logging need the SD card and the strap. | `nos/apps/con_memo.c:47`; `nos/imu_rec.c` | Starting is rejected or fails, and the failure is shown as a toast. |
| low | World-clock favourites use the unchecked `hal_store_save`. | `nos/apps/time_worldclock.c:60` | A failed write is not retried. |
| low | Dead fallback tables `AL[]`, `TL`, `SL`. | `time_alarm.c`, `time_timer.c:334-398`, `time_stopwatch.c` | No user impact; extra code and confusion. |
| low | The world-clock header comment is stale. | `time_worldclock.c:1-5` | Misleading docs. |
| low | Possible spurious compass calibration on first open. | `out_compass.c` (`M->cur.calibrated`) | Unverified. |
| low | Find phone stops when the screen is left. | `con_find.c` `find_leave` | The phone stops ringing as soon as the user goes home. |
