---
title: "OS overview"
tags: [nos, os-map]
updated: 2026-10-23
---

# OS overview

Entry note of the NOS map. Facts come from `docs/os-map/data/graph.json` (253 nodes, 350 edges, 14 flows, 37 schedule rows) and the notes in `systems/` and `apps/`. Where graph.json and a note disagreed, the source in `A:/makerspace/smartwatch2.0` decided (see "Corrections" at the end). Nothing here was measured on a watch.

## What NOS is

- NOS is the custom smartwatch OS for the Waveshare ESP32-S3-Touch-AMOLED-1.43-B: a 466 x 466 AMOLED, FT3168 touch, QMI8658 IMU, PCF85063 RTC, SD card, 16 MB flash and 8 MB PSRAM.
- It is one cooperative UI task (`app_main`, core 0, priority 1) that runs the frame loop, every screen and every service; worker tasks on both cores feed it sensors, storage and radios.
- It draws with a damage-diffed, retained-mode stripe renderer: 40 Hz authored animation clock, 60 Hz present cap, 16-row stripes pushed over QSPI by a flush task on core 1.
- The phone link is Gadgetbridge (Bangle.js protocol) over BLE Nordic UART; Wi-Fi is off by default and the build has no built-in audio, haptic motor or TE line.
- Optional strap modules (heart rate, GPS, LTE, environment, light, compass, haptic, audio) are unverified on this board; without them Heart rate, Navigate, Cellular, Recorder and similar apps show nothing real.

## How the watch breathes

This is one ordinary day, in the order things happen. Cadences are from the source; the table at the end of the section lists every task and timer.

### 1. Boot

`app_main` allocates the DMA stripe buffers first (the radios eat internal RAM), then brings the system up in this order: power-management locks, NVS and peripherals, the panel, the parked flush/touch/cmd tasks plus the `pace` timer, `board_init` (I2C devices, SD mount, saved pedometer, battery calibration; it creates the `board-sense` and `board-fs` tasks), the diagnostic log, legacy `cfg`, **`board_restore_clock`** (the RTC is trusted only if its OS and 12-hour bits are clear), `prefs_load` (SD banks first, NVS fallback), time zone, the calendar gate, the strap layer (`hal_ext_init`), the radios per the saved intent (Bluetooth on, Wi-Fi off by default), the BOOT button interrupt, then `nos_init` and `nos_apps_boot`.

`nos_apps_boot` runs `svc_init` (restores `nosalarm`, `nosact`, `sportstune`, the sports ledger and the workout banks), installs `services_tick` as the engine background hook, makes Home the root screen and pushes the boot screen. The `dotbake` task on core 1 prebakes dot masks and the Solar-face shade table. `wake_service_init` loads `wake_cfg`, the touch and cmd tasks are released, and the UI task prints `READY`. The boot screen is 43 frames at 40 Hz (about 1.08 s): 49 dots form NOS, a touch or the button skips it, then Home takes over. The Wi-Fi weather task and the `wifi-settings` task do not exist yet; they appear only when Wi-Fi is switched on and heap allows.

### 2. The frame loop with the screen on

Each pass of the UI loop, in any display state, does the housekeeping: heap and diagnostic ticks, `radio_preferences` (re-applies the saved Wi-Fi/Bluetooth intent), calendar and display preferences, the queued USB command, the low-battery latch, `wake_service_tick`, the sleep policy and the shutdown state machine.

**Every ON frame** (esp_timer-paced, 60 Hz present cap, 40 Hz authored clock, dt bounded to 100 ms):

- drain the touch ring and the button flags, recognise taps, drags, back-swipes and button presses;
- `nos_frame_run`: knock check, **`nos_bg_tick` = `services_tick` = `svc_tick` + `background_tick`**, transitions, the tick of the top screen(s), banner/toast overlays, `prefs_tick`;
- record the draw list, hash-diff it against the last frame, rasterise only damaged 16-row stripes into DRAM, queue them to the flush task (depth 3) with a DMA fence;
- wait for DMA, pace on the semaphore the `pace` timer releases, and hold `nos-ui` at 240 MHz only while this work runs (80 MHz in the gaps).

**Inside the service tick** (`svc_tick`) each call does: GPS track, voice-memo and SMS-result ticks; `phone_tick` (drain at most 8 lines from the 8 KB NUS ring, send the `ver` handshake, send status every 10 min or on a 5 percent or charger change); the companion queue; music; **a 1 s gate** for `sys_tick` (mirror battery/charger/radio flags), `act_tick` (steps, km, kcal, hourly bins, active minute, checkpoint) and `sports_tick` (60 s journal records); `workout_tick`; **`heart_tick` every 60 s**; **the weather check every 10 s**; stopwatch, timer, `alarm_tick` (fires on a local minute change), `call_find_tick` and `ringer_tick` (haptic and sound every 2 s while something rings). `background_tick` then updates 13 job states at a 1 s gate.

**On timers and slow cadences:** `nosact` saved when dirty after 10 min (5 s retry); the preference checkpoint 2 s after the last edit, 10 s at most, 5 s retry; diagnostic log sample every 30 s; weather fetch every 1800 s (60 s after a failure) once a location exists; SNTP about hourly while Wi-Fi is up; Wi-Fi reconnect backoff 2 s doubling to 300 s; the phone-setup access point stops after 600 s.

**In background tasks, all the time:** `touch` polls the FT3168 (10 ms ON, 40 ms otherwise); `board-sense` runs a 20 ms cycle with about 150 IMU reads per second and feeds the pedometer, wake detector, knock detector, workout motion and wrist detect, samples the battery rail once a second and refreshes the local day key; `board-fs` runs a 100 ms loop (pedometer NVS save, RTC write-back, one CSV log row a minute, SD remount retry); `sports_sd` drains the journal once a second; `cmd` reads USB and the BLE transfer service every 50 ms; `nimble_host` handles BLE events; `ext` services whichever straps exist.

### 3. What continues with the screen off

Light sleep is disabled and there is no tickless idle, so the UI task never blocks on an interrupt. With the display not ON the loop becomes a 20 ms `vTaskDelay` loop (about 50 wakeups per second):

- **OFF:** no frames; once a second it calls `nos_bg_tick(1.0)` and `prefs_tick`; CPU capped at 80 MHz.
- **SENSOR** (the default idle state) and **AOD:** one full frame and one services tick per second, panel at the always-on level; SENSOR caps the CPU at 80 MHz.
- **Still running in every state:** alarms, timer and stopwatch, phone line parsing, step and calorie accounting, workout/track/memo recording, sports journal, battery sampling, IMU reads, raise-to-wake and knock detection, touch polling (samples dropped unless they wake the screen), BLE, Wi-Fi and the weather task, the `ext` task, the radio intent re-apply, the USB command task, RTC write-back.
- **Changes while screen off:** the pedometer NVS save runs only while the display is not ON (the UI clears `board_set_background_persistence` in ON, sets it in OFF/AOD/SENSOR); the preference checkpoint also runs in these loops.
- **Does not happen screen-off:** frames, auto-brightness, the `pace` timer. A notification buzzes but does not wake the panel; only a `SCR_KEEPAWAKE` screen (alarm, timer, call, find-watch ring) forces the display ON within about a second.
- Services freeze only when `mutate` is 0 (an OTA writer is active or shutdown phase 3).

### 4. Sleep and wake

With the shipped defaults (`sleep_s` 15, `aod` 0, `sensor_idle` 1, `fps_cap` 60), 15 s without touch, a keep-awake screen or a finger takes the display to **SENSOR**, never OFF. `power_idle_target` picks SENSOR over AOD over OFF; OFF is reached by the SLEEP action, by the low-battery latch (enter at 15 percent or below, leave at 20 percent or above, cleared by external power), or by turning `sensor_idle` off (its only toggle sits in Battery History).

Wake requests: touch or button always; raise-to-wake needs `raise_wake`, an unlocked watch and no low-battery latch; a knock needs `knock_lock` and no latch (while locked it toggles the lock). `go_state(PWR_ON)` restores the CPU to 240 MHz, resets the sleep timer and swallows the waking touch or press so it never taps. Deep sleep is a different thing: only POWER OFF reaches it, after the shutdown guard (refused during OTA, voice, IMU recording, a running or unsaved workout, an active track, storage dirty), a preference flush, a strap park and final pedometer/RTC/sports flushes. Its only wake source is GPIO0 (BOOT), and wake is a full cold boot. Alarms cannot fire while powered off, and RAM state (timer, stopwatch, notifications) is gone.

### 5. Midnight rollover

Midnight is detected twice, independently, on local date (UTC plus the fixed `tz_min` offset; there is no DST):

1. `board-sense` recomputes the day key at most once a second through the calendar gate. When it changes, `pedometer_update` zeroes today's count and the walk detector restarts. The new 24 B `pedometer` record reaches NVS on the next 60 s `board-fs` save, and only if the display is not ON.
2. `act_tick` on the UI task (1 Hz, also screen off) sees `day != s_as.day` and calls `act_rollover`: it shifts the 7-day week, clears the hourly bins and active minutes, and **saves `nosact` immediately**. A gap above 7 days or a backwards jump starts a clean week.

Also at the day edge: sports journals switch to a new `YYYYMMDD` file, `heart_tick` resets the day min/max, and `alarm_tick` fires a 00:00 alarm on the minute change. A timezone change closes the calendar gate until the new day is settled; a phone `setTime`, SNTP step or manual Set time across midnight is treated as a day change too. If the RTC was untrusted the clock reads 1970 and services gated on a trusted clock go into "not synced" mode while alarms and faces keep running on the bogus time.

### 6. Charging

There is no charge-status pin on the B. "Charging" means external power was inferred: a USB host is seen (`usb_serial_jtag_is_connected`) or the rail reads 4.4 V or more for three consecutive samples. `board-sense` samples the rail once a second (8 ADC reads, median, EMA 1/5), so `svc_sys.charging` follows within a second.

Effects: the low-battery latch clears, so raise-to-wake, knock and the SENSOR/AOD idle target come back; the Dot and Matrix faces show their battery line while charging; the phone gets a status line on the charger change. The percent is a rail-voltage model; `battery_valid` is false on USB, so Battery settings shows `--` while the charger is attached. The screen still times out as usual. Charge current, full and not-charging are unknown.

### Every task and timer

"Any" core means no affinity. Rows marked *(UI task)* are not separate tasks; they are scheduled work inside the UI loop and are listed because they set the cadence of services. Priorities are FreeRTOS numbers; stack sizes in bytes.

| Context | Core | Priority | Cadence | Screen-off behaviour |
|---|---|---|---|---|
| main/UI task, display ON (`app_main`) | 0 | 1 (8192 B) | `pace` timer, 60 Hz present cap, 40 Hz authored clock; services every frame | n/a |
| main/UI task, AOD or SENSOR | 0 | 1 | 20 ms loop (50 Hz); one frame and one services tick per second; 80 MHz in SENSOR | yes, dim or dark panel, sensors stay on |
| main/UI task, display OFF | 0 | 1 | 20 ms loop; `nos_bg_tick(1.0)` once a second; no frames | yes |
| `svc_tick` slow path *(UI task)* | 0 | 1 | `act_tick`, `sys_tick`, `sports_tick` 1 s; `heart_tick` 60 s; weather check 10 s; `nosact` save 10 min when dirty | yes, 1 Hz |
| `prefs_tick` checkpoint *(UI task)* | 0 | 1 | 2 s after last edit, 10 s max age, 5 s retry; only when the persistence window is open | yes |
| `flush` task | 1 | 4 (3072 B) | per dirty stripe; queue depth 3 | idle, nothing to draw |
| `touch` task | 1 | 5 (4096 B) | 10 ms ON, 40 ms otherwise | yes, still polled at 25 Hz; samples dropped unless they wake the display |
| `cmd` task | any | 4 (6144 B) | 50 ms: USB serial, SDXFER inline, BLE XFR poll | yes |
| `dotbake` task | 1 | 1 (3072 B) | boot only, exits when done | n/a |
| `board-sense` task | any | 2 (4096 B static) | 20 ms cycle; IMU reads at 0, 8 and 16 ms (about 150/s); battery sample 1 Hz; calendar day at most 1 Hz | yes, always; stopped only by shutdown |
| `board-fs` task | any | 1 (4096 B) | 100 ms loop; pedometer save 60 s (5 s retry); CSV log row 60 s; RTC write-back when dirty; SD remount retry | yes; pedometer save runs only when the display is not ON |
| `imu-sample` task | any | 3 (4096 B) | 25/50/100 Hz, only while a manual IMU recording runs | continues if started manually |
| `imu-write` task | any | 1 (6144 B) | 20 ms, only while recording | continues if started manually |
| `sports_sd` task | any | 1 (4096 B PSRAM) | 1 s drain, 512-record batch; 20 ms when a query is pending | yes |
| `workout_sd` task | any | 2 (6144 B) | 500 ms; write if dirty and forced or 5 s since last try; checkpoint 30 s | yes |
| `track_save` task | any | 1 (4096 B) | one shot per track stop | yes |
| `memo_scan` task | any | 1 (4096 B) | one shot per Recorder scan | n/a |
| `asset-read` task | any | 1 | on demand (Solar-face textures from SD) | yes |
| `wifi-settings` task | any | 2 (6144 B) | 100 ms; credential checkpoint 5 s after a join; created only when Wi-Fi is switched on | yes |
| `nimble_host` task | 0 | `configMAX_PRIORITIES-4` (4096 B) | event driven (GATT, scan, advertising) | yes |
| `weather` task | any | 2 (8192 B) | 1 s poll; fetch every 1800 s (60 s after failure, 10 s minimum gap); needs Wi-Fi, a stored location and a clock | yes, while Wi-Fi is up |
| `voice` task | any | 3 (10 KB PSRAM) | 1.43C only; compiled as stubs, not created on B | n/a |
| `ota-upload` task | any | 5 (4096 B) | per HTTP `/ota` upload | yes |
| `ota_url` task | any | 5 (8192 B) | per `/ota_url` request | yes |
| `ext` task | any | 0 (4096 B PSRAM) | wakes at most every 50 ms; BME280 2 s; light 400 ms; MAX30102 120 ms with a lease, background read every 10 min (up to 40 s); LTE 1.25 s | yes; modules idle out 8 s after the last getter |
| `exta` audio task | any | 3 (4096 B) | 4 ms I2S blocks while recording or playing | yes |
| `extf` audio file task | any | 0 (4096 B) | on demand | yes |
| `pace` esp_timer | n/a | esp_timer | 1/fps_cap, only while the display is ON | stopped |
| `haptic` esp_timer | n/a | esp_timer | one shot per vibration pattern segment (strap only) | yes |
| `wifi_retry` esp_timer | n/a | esp_timer | 2 s doubling to 300 s while Wi-Fi is wanted and down | yes |
| `wifi_ap` esp_timer | n/a | esp_timer | 600 s one-shot per phone-setup session | yes |
| `airhop` esp_timer | n/a | esp_timer | never created: `hal_wifi_air_start` has no caller in the NOS build | n/a |
| `imu-rate` esp_timer | n/a | esp_timer | 1e6/hz microseconds, only while recording | continues if started manually |
| `ble_control_tick` NimBLE callout | 0 | NimBLE callout | every 100 ms | yes |
| BOOT button ISR (GPIO0) | n/a | ISR | on edge, 25 ms debounce | yes; also the only deep-sleep wake source |
| QSPI DMA-done ISR | n/a | ISR | per DMA transfer | idle |
| deep sleep (ext0) | n/a | n/a | no execution; GPIO0 low causes a cold boot | all sampling stops |

## Data spine

Shared state is held in file-static structs and read through `const` getters, almost all inside `nos/svc.c`; apps copy a snapshot each tick and never own the data. Service state is written only on the UI task. Cross-task inputs (IMU, battery, pedometer, the NUS line ring, the companion queue) cross through short critical sections or mutexes in `board.c` and `hal_esp32.c`.

| Shared state | Owner (writes it) | Fed by | Read by |
|---|---|---|---|
| `nos_prefs` (52 B struct) | prefs service ([[Settings and developer]]) | Settings, Control Center, Face picker, Connectivity, Sports (mass, wrist), phone time (`tz_min`), USB commands | engine every frame, `eng-prefs-poll`, power policy, faces, and most screens |
| Clock and local date | time service on the ESP-IDF system clock | RTC restore at boot, SNTP, Gadgetbridge `setTime`, Set time | home face context (latched per frame), `alarm_tick`, calendar gate, `act_tick`, sports journal, World clock |
| Pedometer count | `core/pedometer.c` inside `board-sense` | QMI8658 (3 Hz-filtered), calendar gate | `svc-activity`, saved by `board-fs` |
| `svc_activity_t` (steps, km, kcal, hourly, week) | `svc-activity` | pedometer, local day | Activity, Energy (kcal), Home widget, Segment/Split/Inverted faces, sports logger, Jobs, phone `act` stream |
| `svc_heart_t` | `svc-heart` | HR strap lease and 10-minute history | Heart rate, Home widget, Split face, phone `act` (`hrm`, no age check) |
| Power: `hal_power` then `svc_sys_t` | battery sampler in `board-sense`; `sys_tick` mirrors it | GPIO4 rail, USB host sense | main-loop power policy (reads `hal_power`), Home/faces, Battery settings, Control Center, Power sheet, phone status, sports journal |
| Notifications (20 entries, PSRAM) | `svc-notifications` | Gadgetbridge `notify`, companion bridge (BLE `0xF2`, HTTP `/notify`, USB) | Notifications screen, banner overlay, Home (unread dot), phone link (DISMISS/OPEN back) |
| Music state | `svc-music` | Gadgetbridge `musicinfo`/`musicstate`, companion bridge | Music app and widget; commands go back via phone link |
| Weather snapshot | `svc-weather` | phone push (valid 3 h) else Wi-Fi fetch | Weather app, Home widget, Split face TMP row |
| Alarms (8) | `svc-alarms` | Alarms app, phone alarm sync | Alarms app, Home (next alarm), ringer |
| Timer, stopwatch | `svc-timer`, `svc-stopwatch` (RAM only) | Timer and Stopwatch apps, Home widget | those apps, widget, ringer |
| Call and find state | `svc-calls`, `svc-find` | Gadgetbridge `call`, `find` | call screen, Find phone and find-watch screens, ringer |
| Ring arbiter | `svc-ringer` | alarm, timer, call, find (priority find > call > alarm > timer) | screen stack (pushes ring screens), haptic, power state (`SCR_KEEPAWAKE`) |
| Sports journal (64 B records) | `svc-sports-logger`, written by `sports_sd` | IMU, battery, activity, wake and heart records | Energy, Battery history, Sensor logs, Jobs, host export tools |
| Workout session | `svc-workout` | Sports app, HR, steps, IMU motion | Sports, Jobs, shutdown guard |
| GPS track | `svc-track` | GPS strap | Navigate, shutdown guard |
| Memo state | `svc-memo` | Recorder, audio strap | Recorder, Home (REC chip) |
| Background job states (13) | `svc-background-jobs` | sensor producers, service ticks | Jobs |
| Strap presence and live values | `drv-ext-hal` and the `ext` task | strap drivers | Modules, Diagnostics, Compass, Barometer, Heart rate, Navigate |
| Diagnostic ring and fault slots | `svc-diag-log`, heap diag | every subsystem | Developer tools, host export |

Two links in the spine are missing by design of the current code: workout `energy_kcal_est` never merges into the daily kcal (steps x 0.04, no resting term), and the Wi-Fi weather path has no way to set its location. See [[Calorie trace]] and [[Connectivity and voice]].

## Persistence map

Reboot here means `esp_restart`. Deep sleep and power loss are cold boots: RAM and RTC_NOINIT are lost, the RTC chip and flash survive.

| Medium | What lives there | Written when | Survives reboot |
|---|---|---|---|
| **SD, authoritative** | `nosprefs` as `/sdcard/watch/settings.0.bin` and `.1.bin` (alternating checksummed 276 B banks, fsync and readback) | `prefs_tick` 2 s / 10 s / 5 s retry | yes; loaded into RAM at boot |
| SD, authoritative | `wifi_cred` as `/sdcard/watch/wifi/settings.{0,1}.bin` | 5 s after a successful join | yes |
| SD bulk | sports journals `YYYYMMDD-events.spt`, `YYYYMMDD-HH-sensors.spt`; workouts `workouts.{0,1}.bin` (30 s checkpoint, RUNNING restores PAUSED); IMU CSV; GPX tracks; WAV memos; diagnostics `diag-00..07.bin`; minute CSV logs; Solar-face textures (read-only, provisioned by hand) | per writer task (see task table) | yes; `.part` memo files are never recovered |
| **NVS** (namespace `wos`, 24 KB) | mirror and card-absent fallback for `nosprefs` and `wifi_cred`, plus `prefs_overlay` | with each SD checkpoint | yes |
| NVS only | `pedometer` (24 B, 60 s, display not ON, plus shutdown flush); `nosact` (92 B, 10 min when dirty and at midnight, not flushed at shutdown); `nosalarm` (164 B, per edit, no retry); `nos_wc` (6 B); `sportsfood` (8200 B) and `sportsmarks`; `sportstune`; `wake_cfg`; `bat_cal`; `ext_sea`; `ext_mag` | per owner | yes |
| NVS, inert | legacy `cfg` (read at boot, never written), `weather_loc` (read, no writer in NOS), orphaned `alarms`, `pip_*`, `lcd_gap`; ESP-IDF `phy` calibration | n/a | yes, but nothing current writes them |
| **RTC chip** | UTC time, BCD (PCF85063) | `board-fs` when the clock changed, and at shutdown | yes; read once per boot |
| **RTC_NOINIT RAM** | diagnostic fault slots, breadcrumbs, eight heap-failure slots | by diag and heap hooks | warm reboot only |
| **Flash** | OTA slots (two `0x780000`, first at `0x20000`), coredump partition, baked fonts (about 281 KB), orrery data pack | OTA / panic / build | yes |
| **RAM and PSRAM only** | notifications (20), timer, stopwatch, snooze, music, weather snapshot, heart history (144 samples), 13 job histories, SMS inbox, call state, GPS points until Stop, layer cache, masks, stripe buffers | n/a | no |

Rules worth remembering: only `nosprefs` and `wifi_cred` go SD-first; a failed SD save stays dirty and retries; a failed final `pedometer` save blocks shutdown; `nosact` can lose up to 10 minutes of hourly steps on a crash or power-off; timers and the stopwatch never persist. Full key table: [[Followup 3 - Complete NVS key and legacy-store ownership including core-apps legacy keys]]; layout and transfer paths: [[Storage and persistence]].

## Key flows

Each list is condensed from `graph.json`. Detail lives in the linked notes.

### Step counting to phone sync
See [[Health and activity]] and [[Followup 1 - Time source, RTC trust and midnight rollover end to end]].
1. QMI8658 runs at 500 Hz; `board-sense` reads it about 150 times a second.
2. `board-sense` filters to 3 Hz and hands the sample to the pedometer, screen on or off.
3. The calendar gate supplies the local day key; the count rolls at local midnight.
4. The pedometer publishes steps, active seconds, walking and valid (valid needs an IMU sample under 250 ms old).
5. `act_tick` (1 Hz) derives km, kcal (steps x 0.04), hourly bins, active minute, 7-day week.
6. Activity and Energy copy the result each tick; Energy's burn bins come from the sports journal.
7. `board-fs` saves the 24 B pedometer record every 60 s while the display is not ON; `nosact` saves after 10 min dirty and at midnight.
8. After the phone sends `{t:act}`, `phone_tick` pushes step deltas and `hrm` over Nordic UART; Gadgetbridge records them.

### Workout recording
See [[Sports and workouts]].
1. Pick a profile on SPORT READY (mass and wrist side come from SETUP), then Start.
2. `workout_tick` samples HR, steps and motion at 1 Hz, also screen off; checkpoint requested every 30 s and on lap or state change.
3. The motion detector finds wrist-movement bursts in the 150 Hz IMU stream.
4. The HR lease is renewed each second and yields bpm only with the unverified strap.
5. `workout_sd` writes a 4304 B checksummed bank with fsync and readback; a RUNNING session restores as PAUSED.
6. IMU, heart and activity records queue to `sports_sd` for the `.spt` journals.
7. FINISH saves history; SESSION DETAILS shows duration, steps, bursts and `energy_kcal_est`; Jobs shows the same row.
8. Missing: `energy_kcal_est` is never merged into daily kcal or the Energy chart, and there is no per-session graph.

### Passive and basal calories (missing link)
See [[Calorie trace]].
1. The profile holds only `sports_mass_kg` and the step goal; mass feeds the workout engine only.
2. Only steps and walking reach the activity service; there is no resting source.
3. Daily kcal is steps x 0.04 with no basal term, so a still wrist burns zero.
4. `sports_tick` journals cumulative step-based kcal every 60 s.
5. Energy's burn bins are increments of that number, so idle hours are empty.
6. Activity's Today page and the Segment face ring show the same value.

### Heart-rate measurement
See [[Heart rate]] and [[Health and activity]].
1. The optional MAX30102 strap produces red/IR FIFO data (unverified on the B).
2. The `ext` task runs the HR service; with a lease it drains the FIFO every 120 ms; the lease expires 8 s after the last request.
3. Opening Heart rate requests the lease, draws a 4 s PPG trace and shows LOCKED BPM.
4. `svc_heart` accepts the locked reading with its acquisition epoch and keeps day min, max and resting.
5. Background reads (first at +2 min, then every 10 min, up to 40 s) fill a 144-sample ring in PSRAM; lost at reboot.
6. `heart_tick` every 60 s rebuilds the 24 h bins.
7. The Home widget and Split face show `bpm_last`; the phone `act` stream sends it with no age check.

### Battery sampling to power states
See [[Power and battery]].
1. GPIO4 reads the system rail through a divider of 3; there is no charge pin.
2. `board-sense` triggers the sampler once a second, screen on or off.
3. The sampler takes 8 reads, median, EMA 1/5, `bat_cal` offset, LiPo curve; valid after 10 stable seconds on battery; USB inferred from voltage.
4. `sys_tick` mirrors percent and charging; status goes to the phone every 10 min or on a 5 percent or charger change.
5. Low-battery latch: enter at 15 percent or below, leave at 20 percent or above, cleared by external power.
6. After `sleep_s` (default 15) idle the loop moves to SENSOR, AOD or OFF per prefs.
7. Brightness drops to the AOD level or the panel gets DISPOFF/SLPIN; SENSOR and OFF cap the CPU at 80 MHz while services keep ticking.
8. `SP_BATTERY` (60 s) and `SP_RAIL` (1 s) records feed Battery history.

### Wake (raise, knock, button, touch)
See [[Followup 2 - Screen-off and low-power run matrix- what runs per display state]].
1. `board-sense` fans filtered IMU samples to the wake service and the knock detector, screen off included.
2. Raise-to-wake runs each loop pass when the display is not ON; it needs `raise_wake`, unlocked, not low battery.
3. A double knock wakes the display (or toggles the lock) when `knock_lock` is set and the battery is not low.
4. The BOOT button edge goes through the ISR with 25 ms debounce.
5. Touch is still polled at 25 Hz and a finger-down blocks the sleep timeout.
6. The power policy decides whether the request is allowed.
7. The UI loop goes to ON: brightness restored, frames resume, CPU to 240 MHz, panel SLPOUT/DISPON, Home plays the wake-from-AOD morph.

### Phone notification
See [[Followup 6 - Gadgetbridge inbound and outbound message audit and phone-link failure modes]].
1. Gadgetbridge sends a `notify` line over Nordic UART.
2. The NUS driver assembles it (up to 2048 B) into the 8 KB PSRAM ring.
3. `phone_tick` drains up to 8 lines per tick (once a second when the screen is off).
4. The parser handles `notify`, `notify~` (update) and `notify-` (remove).
5. The 20-entry RAM store adds it, bumps unread and raises the banner (haptic and sound; the panel does not wake).
6. Pull up from the face for cards; tap for OPEN and DISMISS; swipe or CLEAR ALL.
7. Dismiss removes the entry (nothing persists) and sends `{t:notify, id, n:DISMISS|OPEN}` back to the phone.

### Settings change to checkpoint
See [[Settings and developer]] and [[Storage and persistence]].
1. A row edit writes the `nos_prefs` field and calls `prefs_save()`, which only marks it dirty.
2. `prefs_tick` flushes 2 s after the last edit, 10 s at most, retry 5 s, only when the persistence window is open.
3. Meanwhile the main loop applies the new value live (brightness, sleep timeout, fps cap, radios, time zone).
4. `save_checked('nosprefs')` routes SD first; an alternating 276 B bank is written under the mount lease and SD lock with fsync and readback.
5. NVS `nosprefs` is mirrored as the card-absent fallback; a failed SD save stays dirty and retries.
6. Restart and power off flush prefs before parking.

### Weather (phone or Wi-Fi)
See [[Connectivity and voice]].
1. Phone path: a Gadgetbridge weather JSON (Kelvin temp, code, forecast) reaches the weather service.
2. Wi-Fi path: the `weather` task polls every 1 s and fetches every 1800 s (60 s after failure) when Wi-Fi is up, a location exists and the clock is valid.
3. Broken prerequisite: `weather_loc` must already be in NVS and nothing in the built NOS writes it, so a fresh watch never fetches over Wi-Fi.
4. `wx_tick` every 10 s prefers phone data under 3 h old, else the Wi-Fi snapshot.
5. The Weather app, Home widget and Split face TMP row show the result.

### Alarm or timer while the app is closed
See [[Services hub]].
1. Alarms were saved earlier to `nosalarm`; timers live in RAM only.
2. With the screen off the loop calls `nos_bg_tick(1.0)` once a second.
3. `svc_tick` runs `alarm_tick` (local minute change and weekday) and the timer end check.
4. A match goes to the ringer, which arbitrates find > call > alarm > timer.
5. The ringer pushes the ring screen and repeats haptic and sound every 2 s.
6. The ring screen carries `SCR_KEEPAWAKE`, which forces the display ON.
7. Slide to stop or snooze 9 minutes; the timer offers STOP and +1 MIN; a ring times out after 60 s.

### Boot sequence
See [[Engine and scheduling]].
1. `app_main` starts the pace timer, parked flush/touch/cmd tasks and hardware init.
2. `board_init` initialises I2C devices, touch, IMU and SD, and creates `board-sense` and `board-fs`.
3. `board_restore_clock` trusts the PCF85063 (OS and 12-hour bits rejected).
4. Preferences load from the SD banks, falling back to NVS; legacy `cfg` loads too.
5. `nos_apps_boot` runs `svc_init` (restores `nosact`, `nosalarm`, sports and workout state) and pushes the boot screen.
6. `dotbake` prebakes dot masks and the orrery shade table on core 1.
7. The boot screen runs 43 frames (about 1.1 s); a touch or button skips it.
8. Home takes over with the face build-in; the face page is the root.

### Frame: input to panel
See [[Rendering pipeline]].
1. The `touch` task polls the FT3168 every 10 ms; native coordinates map to the rotated UI as (465-y, x).
2. The UI loop drains the 16-entry ring and picks up button flags.
3. The input engine recognises taps, drags, back-swipes and button presses.
4. `frame_run` calls `nos_bg_tick`, then transitions and the top screen's tick (dt bounded to 100 ms).
5. Screens draw into the command record; hashes are diffed so only damaged 16-row stripes are rasterised into DRAM.
6. A valid retained layer is blitted instead of redrawn; live blur is gated off on the device (`NOS_RENDER_SHARP=1`), only the notification-card blur cache still bakes.
7. Stripe jobs and a fence go to the flush task (depth 3), which rotates each stripe CCW into DMA buffers and draws over QSPI at 80 MHz.
8. The DMA-done credit returns; there is no TE wait on the B, so tearing is possible. The pacer then waits for the next slot.

### Time source, RTC trust and midnight rollover
See [[Followup 1 - Time source, RTC trust and midnight rollover end to end]].
1. At boot the PCF85063 supplies UTC as BCD; the OS and 12-hour bits are rejected.
2. `board_restore_clock` sets the system clock.
3. SNTP corrects it about hourly while Wi-Fi is connected.
4. Phone `setTime` and `E.setTimeZone` overwrite the clock and `tz_min`; Set time and the timezone picker change them too.
5. Any change raises `board_clock_changed`; `board-fs` writes the RTC back (forced at shutdown).
6. The calendar gate derives the local date and closes while the time zone changes.
7. `act_rollover` archives the day and saves `nosact` immediately.

### USB command and SD transfer
See [[Followup 5 - USB command to handler map and host protocol edge cases]].
1. A host tool (`flash_watch`, `sdsync`, an export script) sends a framed text command.
2. The USB link frames on LF (191 B cap, NUL+LF resync) and supports the `req <id>` envelope.
3. The `cmd` task answers SDXFER verbs inline every 50 ms and queues other lines.
4. The UI loop executes about 130 verbs: prefs, Gadgetbridge injection, strap debug, `batcal`, diagnostics, open screen.
5. SD transfer verbs list, get and put files under the mount lease; mutation is blocked during OTA.
6. The checked flasher checkpoints preferences first so settings survive a flash.

## Systems

Each note is a source review of one subsystem.

- [[Engine and scheduling]] - the single UI task, frame loop, screen stack, input, pacing and the `mutate` admission gate.
- [[Rendering pipeline]] - recorded draw list, damage diff, 16-row stripes, flush task, QSPI panel path, sharp-only device build.
- [[Hardware and drivers]] - board chips, I2C/SD/RTC/IMU drivers, `board-sense` and `board-fs`, HAL contract and the strap layer.
- [[Services hub]] - `svc.c` getters and ticks: notifications, music, weather, alarms, timer, stopwatch, calls, ringer, phone link.
- [[Health and activity]] - pedometer, activity totals, heart, wrist detect; the steps x 0.04 calorie model.
- [[Sports and workouts]] - workout sessions, the always-on sports journal, the manual IMU CSV recorder and host export.
- [[Calorie trace]] - why a badminton session showed no calories, no graph and no basal energy.
- [[Power and battery]] - rail sampling, power states, DFS, low-battery latch, shutdown, power findings.
- [[Storage and persistence]] - NVS, SD, RTC RAM, transfer protocol, OTA slots and the checked flasher.
- [[Connectivity and voice]] - BLE/Gadgetbridge, Wi-Fi, HTTP, SNTP, weather fetch, LTE, OTA, voice (stubbed on the B).
- [[Settings and developer]] - `nos_prefs`, the SD-first checkpoint, Settings, Modules, Diagnostics, Developer tools, diagnostic log.
- [[Shell and faces]] - boot, Home pager, Control Center, Notifications, power sheet, face picker and the nine faces.
- [[Utility apps]] - Alarms, Timer, Stopwatch, World clock, Weather, Compass, Barometer, IMU, Navigate, Music, Find phone, Torch, Recorder.
- [[Followup 1 - Time source, RTC trust and midnight rollover end to end]] - every clock writer, RTC trust rules, calendar gate and the two midnight detectors.
- [[Followup 2 - Screen-off and low-power run matrix- what runs per display state]] - run matrix per display state, deep sleep, alarms while off.
- [[Followup 3 - Complete NVS key and legacy-store ownership including core-apps legacy keys]] - every NVS key, owner, size, writer and legacy leftovers; NVS capacity model.
- [[Followup 4 - Uncovered built files- blur pipeline, orrery shade, fonts]] - blur modules (mostly dead on device), orrery shade table and baked fonts.
- [[Followup 5 - USB command to handler map and host protocol edge cases]] - the roughly 130 USB verbs, framing, SD transfer and OTA edge cases.
- [[Followup 6 - Gadgetbridge inbound and outbound message audit and phone-link failure modes]] - every GB message in and out, the companion bridge and phone-link failure modes.

## Apps

Status is the note's own source-review verdict: **working**, **partial** or **stub**. Nothing was bench-tested. Screens that are not launcher tiles (system screens, faces) are included.

### Shell and system screens

- [[Boot screen]] (working) - 1.08 s dot-matrix NOS intro, then Home.
- [[Home pager]] (working) - root screen: seven pages (Activity, Heart, Weather, Music, Timer widgets, Face, Apps).
- [[Launcher]] (working) - 28-entry app list, embedded as Home page 1.
- [[Control Center]] (partial) - pull-down sheet of eleven tiles and a brightness pill.
- [[Notifications]] (partial) - pull-up feed of up to 20 cards with OPEN and DISMISS.
- [[Power - screen-off screen]] (partial) - button-hold sheet with SLEEP, RESTART and POWER OFF.
- [[Incoming call screen]] (partial) - accept, decline and end as phone commands; no call audio.
- [[Face picker]] (working) - full-size paging preview of the nine faces; writes `nos_prefs.face`.
- [[Face registry and shared face kit]] (working) - face table, contract and the shared drawing kit.

### Watch faces

- [[Dot face]] (working) - default face: HH over MM in a 5x9 dot font with a red seconds dot.
- [[Matrix face]] (working) - 25 x 25 dot grid with seconds ring and sparkle.
- [[Glyph face]] (working) - analog face with dot ticks, capsule hands and a red second dot.
- [[Split face]] (working) - typographic hours/minutes with HR, STP, TMP and BAT rows.
- [[Life face]] (working) - hour as a grid of minute dots, day as a ring of 24 hour dots.
- [[Segment face]] (working) - three 60-segment rings for steps, kcal and active minutes.
- [[Inverted face]] (working) - white-disc face with a black dot-matrix time and step count.
- [[Tape face]] (working) - cassette reels across the day, timecode; the recording lamp does not do what the header says.
- [[Orrery - Solar face]] (working) - solar system at the current time with its own ephemeris, pan/zoom/time travel and a planet card.

### Time and utility

- [[Alarms]] (partial) - alarm list, editor and ring screen; logic lives in the services, saved as `nosalarm`.
- [[Timer]] (partial) - countdown setup, run view and TIME'S UP; RAM only.
- [[Stopwatch]] (working) - start, stop, lap, reset; survives leaving the screen, lost at reboot.
- [[World clock]] (working) - up to four favourite cities from 13, fixed offsets with six DST rules.
- [[Weather]] (partial) - three-page forecast and Home widget over phone or Wi-Fi data.
- [[Music]] (partial) - remote control for the phone's media player; the watch plays no audio.
- [[Find phone]] (partial) - one-button findPhone plus the reverse find-watch ring; the phone never confirms.
- [[Torch]] (working) - the whole panel as a white or red light.
- [[Recorder]] (partial) - voice memos to WAV and playback; needs the unverified mic and speaker strap.
- [[Chat]] (stub) - push-to-talk assistant UI; on the B it only says it needs the C board mic and speaker.

### Fitness and sensors

- [[Activity]] (working) - steps hero, today and 7-day pages over `svc_activity`.
- [[Energy]] (partial) - viewer over the sports journal with food/water ledgers; the ENERGY total is steps x 0.04.
- [[Heart rate]] (partial) - live BPM, PPG trace and 24 h history; needs the MAX30102 strap.
- [[Breathe]] (partial) - 4-2-6 paced breathing sessions; optional HR strap.
- [[Sports]] (partial) - workout UI with 40 profiles; needs SD history loaded; GPS and HR need straps.
- [[Jobs]] (working) - read-only board of 13 background jobs with 2-minute histories.
- [[IMU]] (partial) - live accel/gyro, CSV recording at 25/50/100 Hz, file browser.
- [[Compass]] (partial) - magnetometer heading, figure-8 calibration and bubble level; strap only.
- [[Barometer]] (working) - pressure, altitude and climate from the BME280 strap; sea-level picker.
- [[Navigate]] (partial) - GPS sky plot, track recording to GPX and back-to-start bearing; needs the GPS strap.
- [[Cellular]] (partial) - LTE strap status, radio switch and SMS reader; unverified hardware.

### Settings and developer

- [[Settings]] (partial) - 30-row preference list with picker, slider and Set time pages.
- [[Connectivity settings]] (partial) - Wi-Fi and Bluetooth lists, password entry and phone setup.
- [[Battery settings]] (partial) - read-only level, source and rail voltage; percent is an estimate.
- [[About]] (partial) - version, board, free memory, uptime and font credits.
- [[Modules]] (partial) - 16-part hardware status board with per-part tests; reached from Settings.
- [[Diagnostics]] (partial) - guided 15-part self-test with PASS/FAIL/ABSENT/SKIP results.
- [[Developer tools]] (partial) - on-screen overlays, frame-rate cap and system trace; the TE row is inert on the B.

Counts: 46 app notes, 20 working, 25 partial, 1 stub. Across all 253 graph nodes: 165 working, 61 partial, 9 unknown (strap hardware), 9 dead-code, 8 stub, 1 broken (the Wi-Fi weather fetch).

## Health of the OS

Confirmed gaps by severity: **high 4, medium 40, low 102** (146 in total). The list, with evidence and impact for each, is in [[Gaps]].

What the graph itself marks: one **broken** node (`svc-weather-fetch`: no way to set a location in the built NOS), nine **dead-code** nodes (live blur modules, `airhop` timer, battery-log ring, legacy config and `core/apps` leftovers), eight **stub** nodes (voice, TE sync, SMS inbox, Cellular tick and similar), and nine **unknown** nodes (the eight strap modules, unverified on this board, and the Open-Meteo API).

## Corrections made against the source

- The graph's boot flow says `svc_init` starts the weather task. In the source (`hal_esp32.c`, radio loop) the task starts only when Wi-Fi is initialised and enabled and heap admission passes; Wi-Fi is off by default.
- The Shell note calls the boot screen about 1.4 s. The code is 43 frames at 40 Hz, about 1.08 s (`sys_boot.c`, `render_config.h`).
- One note puts the `board-sense` stack in PSRAM; the source allocates a 4096 B internal stack (`nos_mem_alloc`) because that task must not touch flash.
