---
title: "Time source, RTC trust and midnight rollover end to end"
tags: [nos, os-map]
updated: 2026-10-23
---

# Time source, RTC trust and midnight rollover end to end

Scope: 1.43-B NOS build (`BOARD_HAS_RTC 1`, `firmware/main/pins.h:62`; the 1.43C has `BOARD_HAS_RTC 0`, `pins.h:28`). Paths are repo-relative. "Code does" statements come from reading source; docs were not trusted.

## Summary

- There is ONE time-of-day chain: the ESP-IDF/newlib system clock (`time()/gettimeofday()`, UTC) plus a process-wide `TZ` string built from a fixed minute offset (`UTC-5:30` style, no DST). Everything displays `localtime_r()` of that (`hal_get_datetime`, `firmware/main/hal_esp32.c:116-146`). `hal_epoch_seconds()` (`hal_esp32.c:148-152`) is the "trusted UTC" gate: 0 when the clock is before 2024-01-01 (1704067200) or after 2100.
- The PCF85063 RTC (I2C 0x51) is read ONCE per boot (`board_restore_clock`, `board.c:847`) to seed the system clock and is WRITTEN (never read again) whenever any source changes the clock (`board_clock_changed` -> dirty flag -> `rtc_maintain` -> `rtc_save`, `board.c:747-828`). It stores UTC, BCD, 24 h mode.
- Writers of the system clock (last writer wins, no arbitration): RTC restore at boot; Gadgetbridge `setTime()` (`nos/svc.c:1000-1019` -> `hal_set_datetime`); SNTP over Wi-Fi (lwIP sets the clock, `ntp_synced` then calls `board_clock_changed`, `hal_esp32.c:1835-1848`, hourly via `CONFIG_LWIP_SNTP_UPDATE_DELAY=3600000`, `firmware/sdkconfig:2201`); legacy BLE "TIME" characteristic (`hal_esp32.c:847-885`); Settings SET TIME (hour/minute only, `nos/apps/set_settings.c:2287-2381`).
- "valid" vs "trusted" in `board.c`: `s_rtc_valid` = the RTC chip's last read/write succeeded (cleared by any RTC I2C failure, `board.c:763`; it only feeds `HAL_FEAT_SYNC`, which NOS never reads). `s_system_trusted` = the system clock holds a real date (RTC read OK, retained clock >= 2020, or a `board_clock_changed` epoch within 2000..2100); it gates writing the RTC so a bogus 1970 time is never seeded into it (`board.c:750-751, 816, 873, 878`). A third gate drives the pedometer calendar (>= 2020-01-01, `board.c:158`) and a fourth drives UI/service logic (`hal_epoch_seconds`, >= 2024).
- Midnight is detected independently in two unsynchronised places: the sensor task (`calendar_day`, date recomputed each 1 s, resets the pedometer) and the UI-task service `act_tick` (1 Hz; owns hourly bins, 6-day week, `nosact`). Both key on the LOCAL date, so ANY change of local date (midnight, phone correction, timezone change, manual set across midnight) is treated as a day change.
- RTC-less fallback (power loss, OS flag set, or never set): `settimeofday(0)` (`board.c:884-886`): faces show 1970-01-01 at the configured tz (default +330, so 05:30) plus uptime, with no on-screen indicator; UI/services gated on `hal_epoch_seconds()` go into "not synced" mode, but alarms, faces, sports minute stamps and world clock keep running off the bogus clock. Manual SET TIME is rejected while year < 2000 ("DATE NOT SET: SYNC PHONE") and cannot edit the date.

## Components

### hw-pcf85063-rtc (PCF85063 on the shared I2C bus)
- Purpose: battery-backed UTC calendar, address 0x51; INT on GPIO15 (`PIN_RTC_INT`, `pins.h:56`) is defined but never used (no alarm, no wake).
- Where it runs: accessed only from `board.c` through i2c handle `s_rtc` under the `BOARD_DEVICE_RTC` lock, 30 ms timeout; boot read on the boot task, writes on `task-board-fs`.
- Cadence: one read per boot; writes only when dirty.
- Reads/writes: reg 0x00 control (STOP 0x20, 12/24 bit 0x02), regs 0x04..0x0A seconds (bit7 = OS)/min/hour/day/weekday/month/year in BCD.
- Files: `firmware/main/board.c:433, 767-799, 847-889`, `firmware/main/board_protocol.c:26-56`.

### svc-rtc-sync (RTC trust, BCD codec, write-back) in board.c
- Purpose: restore system clock from RTC at boot; mirror every clock change into the RTC; keep requests pending across failures.
- State (`board.c:53-72`): `s_rtc_pending` (epoch of last change, informational), `s_rtc_dirty`, `s_rtc_generation` (a newer notification cannot be cleared by an older successful write), `s_rtc_retry_at` (esp_timer us), `s_system_trusted`, atomic `s_rtc_valid`, failure telemetry `s_rtc_failures/last_error/failed_operation`.
- BCD path: `board_rtc_decode` rejects OS bit (0x80) or weekday > 6, non-BCD nibbles, out-of-range month/day, years 2000..2099; it does not cross-check weekday against the date. `board_rtc_encode` accepts epoch 946684800..4102444800 only and writes weekday `(days+4)%7` (`board_protocol.c:26-56`).
- `board_restore_clock` (`board.c:847-889`): (1) `board_apply_timezone()`; (2) under the device lock read control, accept only if STOP and 12 h bits (mask 0x22) are clear, read 7 time regs, decode; a read error clears `s_rtc_ok` and schedules a 1 s retry; (3) valid: `settimeofday(epoch)`, `s_rtc_valid=1`, `s_system_trusted=1`, log "RTC: restored UTC time"; (4) else if retained system clock >= 2020 (survives soft reset in the IDF RTC timer per the in-code comment): trusted=1 and `board_clock_changed(time(NULL))` so the RTC self-heals; (5) else `settimeofday(0)` and log "RTC: unset; settime over USB or sync phone/WiFi"; RTC is NOT seeded. Called once at boot before radios start (`nos_main.c:1922`; comment at `board.c:864-865`).
- `board_clock_changed(epoch)` (`board.c:747-754`): under `s_state_lock` sets pending/dirty, `generation++`, `s_system_trusted = epoch in [2000,2100)`, `retry_at=0`. Callable from any task (SNTP on lwIP tcpip task, NimBLE host task, UI task).
- `rtc_maintain(force)` (`board.c:801-828`): returns if not dirty; if due (now >= retry_at or forced) and trusted calls `rtc_save(time(NULL))` (CURRENT UTC, not the stale epoch); on success clears dirty only if generation unchanged; on failure `retry_at = now + 5 s`. With `!BOARD_HAS_RTC` it only clears dirty.
- `rtc_save` (`board.c:767-799`): read control, clear 0x22, write STOP, burst-write 7 time bytes from reg 0x04 (seconds byte bit7=0 clears OS), on failure write 0x80 to the seconds reg so a partial calendar stays invalid, always try to restore control, `s_rtc_valid=1` on success else `rtc_failure` (logged once per distinct error; request retained). Seconds resolution; STOP/restart resets the sub-second divider each write.
- `board_clock_valid()` (`board.c:840-846`): returns `s_rtc_valid` (no-RTC boards upgrade it when `time() >= 2020`).
- Runs: `task-board-fs` (`board_fs_task`, prio 1, internal-RAM stack 4096, unpinned, 100 ms loop; `rtc_maintain(0)` at `board.c:957`). In shutdown it calls `rtc_maintain(1)` (`board.c:935`) and `board_prepare_sleep` refuses while `s_rtc_dirty` (`board.c:1146-1154`). Screen-off independent.

### svc-time-hal (system clock, TZ and setters in hal_esp32.c)
- `get_datetime_raw`/`hal_get_datetime` (`hal_esp32.c:116-146`): `gettimeofday` + `localtime_r` into `hal_datetime` (weekday = tm_wday). `hal_frame_begin()` latches one datetime per frame, `hal_frame_end()` releases it (`:132-138`); `nos_frame_run` brackets the whole frame including `nos_bg_tick` (`nos/nos.c:2321, 2341, 2464`), so all services in a frame share one civil time. `hal_epoch_seconds()` is live, not latched.
- `hal_set_datetime_checked(dt)` (`hal_esp32.c:154-173`): `board_datetime_valid` (year 2000..2099 etc., `board_protocol.c:19-25`) -> `board_apply_timezone()` -> `mktime` -> `settimeofday` -> `board_clock_changed(tv_sec)` -> `g_synced=1`. This is the only NOS-side clock writer and it reaches the RTC (within ~100 ms via `task-board-fs`).
- `apply_tz(off_min)`/`hal_set_tz_min` (`hal_esp32.c:822-834`): range -720..840; sets `g_tz_offset_min` (`core/settings.c:20`, default 330) and `setenv("TZ","UTC<-/+>h:mm")` (POSIX west-positive), `tzset()`. `board_apply_timezone()` (`board.c:830-838`) does the same from `g_tz_offset_min` and first closes the pedometer calendar gate (`board_calendar_ready(0)`).
- `time_access` BLE write on the legacy watch-service TIME characteristic (`hal_esp32.c:847-885`): `[epoch:4 LE UTC]` (2000..2100 only), optional `[tz_min:2]`, optional lat/lon/city; `settimeofday` + `board_clock_changed` + `g_synced=1`; `apply_tz(tzm)` runs on the NimBLE host task and changes only `g_tz_offset_min`.
- `ntp_start` (`hal_esp32.c:1842-1850`, called at `IP_EVENT_STA_GOT_IP`, `:1980`): `pool.ntp.org`, poll mode; `ntp_synced` does `board_clock_changed`, `g_synced=1`. IDF default immediate step (no slew), 1 h poll.
- `hal_time_resync()` (`hal_esp32.c:1057-1068`): `hal_weather_refresh()`, legacy cmd 6 + BLE notify to the companion, `esp_sntp_restart()` if Wi-Fi is up; sends nothing to a Gadgetbridge (NUS) phone. Triggered by Modules > RTC > RESYNC (`nos/apps/set_modules.c:1372-1374`).

### svc-gadgetbridge time frame (gb.c + svc.c apply_time)
- Parse: `gb_frame_line` (`nos/gb.c:96-149`) finds `setTime(n)` (`gb.c:138-142`: number > 0 and < 2^63, `epoch=(int64)v`, fraction truncated) and `setTimeZone(h)` (`gb.c:143-147`: -14..+14 h, float). Plain JS-call text, not JSON.
- Dispatch: `handle_line` calls `apply_time` whenever `has_time || has_tz` (`svc.c:1195-1197`); `phone_tick` drains up to `LINES_PER_FRAME` lines per service tick (`svc.c:1236-1241`).
- `apply_time` (`svc.c:1000-1019`): `tz = round(hours*60)` (default current `nos_prefs.tz_min`); out of -720..840 drops the whole frame including the time; if changed sets `nos_prefs.tz_min` and `prefs_save()` (SD authoritative `/sdcard/watch/settings.0.bin|1.bin`, NVS `nosprefs` mirror); ALWAYS `hal_set_tz_min(tz)`; if `has_time && epoch >= 946684800` converts epoch+tz to civil fields (`civil_from_days`) and `hal_set_datetime`. weekday is computed but ignored. Round trip UTC -> local civil -> `mktime` -> UTC is exact for a fixed offset; sub-second lost.
- Cadence: whenever the phone sends it. In-repo evidence only shows the connect-time push (`sim/nos_phone_sim.c:96-103`, `tools/gb_test.c:118-125`); whether Gadgetbridge also pushes on phone time/zone change is not verifiable here. USB: serial `gb LINE` injects a line via `svc_phone_inject` (`svc.c:1202`, help `nos_main.c:1789`).
- Runs in the UI task via `svc_tick` (1 Hz when screen off through `nos_bg_tick`, `nos_main.c:2094`).

### app-settings: scr_set_time and the timezone picker
- `scr_set_time` (`nos/apps/set_settings.c:2287-2381`): two drums (hours 24, minutes 60) seeded from `hal_get_datetime`. SET copies the current `hal_datetime`, replaces hour/minute, forces second=0 and millis=0, calls `hal_set_datetime_checked`; success -> "TIME SET" + pop; failure -> "DATE NOT SET: SYNC PHONE" if `dt.year<2000||>2099`, else "TIME NOT SET: TRY AGAIN". No date editor exists in NOS.
- TZ picker (`set_settings.c:386-390, 1905-1940`): 39 fixed offsets (-720..+840). `pick_commit` writes `nos_prefs.tz_min` and `prefs_save()`; it does not touch the clock (`pick_leave` comment, `:1959-1963`). The firmware loop applies it: `calendar_preferences` (`nos_main.c:962-966`), run every loop iteration incl. screen-off (`nos_main.c:1987`). Preview uses `hal_epoch_seconds()`; shows "TIME NOT SYNCED" when 0.
- Runs in the UI task on touch.

### svc-calendar-gate (board.c calendar_day, pedometer day source)
- `board_calendar_ready(ready)` (`board.c:115-127`): short-lock gate. Closing snapshots the last complete pedometer (`s_calendar_pending_base`), bumps `s_calendar_closed_generation`, bumps revision, clears `s_calendar_sample_ready`. `nos_main.c:965` re-opens it every loop with `!prefs_loading()`; `calendar_day` also closes it while `prefs_loading()` (`board.c:139`).
- `calendar_day(now_us)` (`board.c:134-177`): returns 0 (held) when the gate is closed, epoch < 2020 or >= 2100, or `localtime_r` fails. Otherwise recomputes at most every 1 s (`next_date_us`) or on revision change: `day = Y*10000+M*100+D` of local time. A "deferred" seed from a closed gate keeps only samples accumulated since the seed when the date differs from the seed's day; same date keeps the count.
- Consumer: `task-board-sense` calls `pedometer_update(&s_pedometer, day, ...)` every 20 ms (`board.c:988-989`); `core/pedometer.c:9-21`: `day && today.day && day != today.day` zeroes the day record and re-inits the walk detector. No direction check, so a backward date change also zeroes.
- `s_calendar_sample_ready = (day != 0)` and `s_activity.valid = 0` while unresolved (`board.c:1011-1012`), so `act_tick` sees `valid==0` and publishes only `max_steps`.
- Persistence: `save_pedometer` (`board.c:903-917`) writes NVS blob `pedometer` (`pedometer_saved{version=1,day,steps,active_ms}`, namespace `wos`) every 60 s (5 s retry) when changed and only when the calendar is ready; also at shutdown (`board.c:932-934`). Loaded in `board_init` before TZ/prefs (`board.c:1066-1068`); accepted day range 20200101..20991231 (`core/pedometer.c:7`).
- Runs: `task-board-sense` (`board_task`, prio 2, `tskNO_AFFINITY`, 4096-byte PSRAM stack, 8+8+4 ms delays per cycle, `board.c:967-1020`, created `board.c:1030-1042`); `task-board-fs` for saves.

### svc-activity (act_tick in nos/svc.c): hourly bins, rollover, 6-day week
- Firmware path `svc.c:480-577`; the `#ifndef ESP_PLATFORM` block at `svc.c:461-478` is a scripted simulator day.
- `act_tick(now)` once per second from `svc_tick` (`svc.c:1310-1319`) in the UI task, also in screen-off/AOD (below). `!a.valid` -> publish `max_steps` only. With `hal_epoch_seconds()!=0`: `day = day_key(hal_get_datetime())`; first ever -> `s_as.day=day`; `day != s_as.day` -> `act_rollover`; `steps < last_steps` -> `last_steps=steps` (pedometer reset); delta -> `hourly[dt.hour]` (u16 saturating); `max_steps` monotonic; active minute when > 60 steps in a minute.
- `act_rollover` (`svc.c:511-528`): `gap = key_days(new)-key_days(old)`; `gap<0 || gap>7` -> `gap=7` ("clock jumped: start a clean week") which wipes week and valid_days; otherwise shift `week[]` per day (first shift receives `max_steps`, later zeros); clear hourly and active minutes; `max_steps=last_steps=steps` (the value passed in); `s_as.day=day`; immediate `act_save()`.
- Persistence: NVS blob `nosact` (`act_saved` v2: day, week[6], hourly[24], active_min, last_steps, max_steps) at rollover, every 10 min when dirty (`svc.c:558`), 5 s retry after failure. NOT flushed at shutdown.
- Readers: `fit_activity.c:50-52` (week bar labels from `dt.weekday`), `sys_home.c:469-473` (face steps), `sports_tick` SP_ACTIVITY each 60 s (`sports.c:386-392`), GB realtime `act` messages (`svc.c:1140-1146, 1243-1246`).

### svc-sports-logger (nos/sports.c): date-keyed journals
- `sports_tick` (1 Hz from `svc_tick`, `sports.c:371-380`) snapshots `clock_epoch = hal_epoch_seconds()`, `clock_ms`, `clock_day = epoch ? local YYYYMMDD : 0`, `clock_minute = local hour*60+minute` (set even when unsynced). `record()` (`sports.c:111-123`) stamps `day`, `minute`, `epoch = clock_epoch + (uptime-clock_ms)/1000` (0 unsynced); the snapshot can be up to 1 s stale at a midnight crossing.
- Writer `task-sports-sd` ("sports_sd", prio 1, PSRAM stack 4096, unpinned; `sports.c:316-322`) drains every 1 s under `hal_sports_sd_lock` into `/sdcard/watch/sports/%08lu-events.spt` and `/sdcard/watch/sports/%08lu-%02d-sensors.spt` (hour = minute/60) (`sports.c:25, 215-218, 255-262`); `00000000` when unsynced. File is switched when `day` (events) or `day*24+hour` (raw) changes; opened `r+b`, appended after the last whole record.
- `sports_day(offset)` (`sports.c:102-109`) builds the Energy app's day key from the live local date via `mktime`; unsynced it returns 19700101 while journals are under 00000000 (`fit_energy.c:34`). Intake ledger `sportsfood`/`sportsmarks` (NVS) carry `day` and are replayed into the journal at boot (`sports.c:317-318, 387-391`).

### svc-alarms and svc-timer/stopwatch
- Alarm firing (`svc.c:826-851`, UI task, every frame; 1 Hz when screen off): `key = dt.day*1440 + hour*60 + minute` from the latched local datetime; on key change (initial `s_alarm_key=-1`, `svc.c:90`) every enabled alarm with matching h:m and weekday bit (`1<<dt.weekday`, Sunday=0) rings; `days==0` = one-shot, turned off and `svc_alarm_save()` (NVS `nosalarm`). No `hal_epoch_seconds()` guard, no catch-up, re-evaluates after any backward step.
- `svc_alarm_next` / `al_in_min` (`svc.c:777-813`, `time_alarm.c:105-118`) scan 7 days from the current local weekday; snooze uses `hal_millis()`.
- Timer/stopwatch/snooze/ring timeouts use only `hal_millis()` (`svc.c:857-912`) and are immune to clock jumps.
- Shutdown: `esp_deep_sleep_start` with only GPIO0 as wake (`nos_main.c:810-811`); the PCF85063 alarm is never programmed, so nothing wakes for an alarm.

### app-world-clock (nos/apps/time_worldclock.c)
- `utc_now` = `hal_epoch_seconds()` else local fields minus `nos_prefs.tz_min` (`:122-129`); `local_offset_min` = (local civil seconds - epoch) rounded to minutes, fallback `nos_prefs.tz_min` (`:133-141`) so a tz change is followed automatically; city DST rules (EU/US/EG/AU/NZ) evaluated against UTC (`:100-120`); `city_time` wraps minutes into `dayrel`/weekday (`:143-158`). No per-day store.

### svc-heart (heart_tick + ext_hr epoch stamps)
- `firmware/main/ext/ext_hr.c:216-227, 289-295`: each reading and history entry carries `hal_epoch_seconds()` (0 unsynced); history is a 144-entry RAM ring of `hal_hr_sample{sample_ms,epoch,bpm}` (`ext_hr.c:36, 61`), lost at reboot.
- `heart_tick` (every 60 s, `svc.c:586-623`): local day change via `day_key` resets `s_hr_rec_day_min/max`; bins the last 24 h into 144 x 10 min by `(ep-at)/600`; skips `at>ep` and `ep-at>=86400`; `midnight = ep - minutes_today*60 - second` from the latched datetime; min/max today use `at>=midnight`.
- `svc_heart_record_at` (`svc.c:625-643`) drops any acquisition older than `bpm_last_epoch` and any later than `now`.

### Faces and home (face_ctx datetime)
- `sys_home.c:468` `build_ctx` latches `hal_get_datetime(&c->dt)` once per frame into `face_ctx`; every face reads time from `c->dt`. The date line `fk_date` = weekday3 + day-of-month only (`nos/faces/faces.c:107-110`), used by `face_dot.c:66`, `face_inverted.c:74`, `face_tape.c:91`, `face_glyph.c:115`, `face_life.c:94`, `face_segment.c:258`, `face_orrery.c:1887`. No month/year, no unsynced marker, no epoch check, no cached date state: at local midnight the text changes on the next rendered frame (AOD runs `nos_frame_run` at 1 Hz, `nos_main.c:2108-2117`). AOD pixel shift uses `hour*60+minute` (`faces.c:28-32`).
- Sync state is exposed only in `set_modules.c:774-778` ("UTC SYNCED/NOT SYNCED" from `hal_epoch_seconds()`), the TZ picker preview, and Diagnostics' clock test (`set_diag.c:200-215`, which compares the SYSTEM clock with uptime, not the RTC chip).

### face-orrery and orrery card
- `face_orrery.c:1141-1146`: `unix_now = hal_epoch_seconds() + millis` when trusted, else the LOCAL civil time treated as UTC. `tz_days` (local minus UTC in days) is recomputed only when trusted (`:1183-1186`) and keys ring/calendar caches (`:1022-1044, 2145-2146`). `S.jd_now = ep_jd_unix(un)` every tick; travel offset `S.off` is relative, so a clock jump moves the whole sky. `orrery_jd()/orrery_now()` (`:2633-2634`) feed `orrery_events.c:53` (events filtered `jd >= now-0.5` when the list is built) and the card.
- `orrery_card.c:263-266, 1930-1931`: `card_jd = open_jd + (now-open_epoch)/86400`; if opened while unsynced (`open_epoch==0`) the card clock never advances that session. The face ticks only while displayed; AOD forces `locked` at now.

### app-recorder (con_memo.c) and workouts
- Memo WAV name `M_YYYYMMDD_HHMMSS_NNNN.wav` is UTC (`gmtime` of `hal_epoch_seconds()`, or local fields minus tz when unsynced) (`con_memo.c:434-453, 592-600`); the list re-derives the displayed local date from the name and the CURRENT `tz_min` on each scan (`con_memo.c:195-208`).
- Workout history prints the saved start as UTC with a `Z` suffix, "TIME NOT SET" when `started_epoch==0` (`fit_workout.c:827-843, 1027`); `workout.c:207-211` accepts epochs >= 2020. Workouts/IMU/GPX files are slot/number keyed (`workouts.%d.bin` `workout.c:523`, `imu/%04d.csv`, `%04d.gpx`), not date keyed.

### Other epoch consumers (no day logic)
- `weather_service.c:96` waits for `hal_epoch_seconds()` before HTTPS (cert dating); weather day labels use the forecast's `utc_offset_seconds`, not the watch tz (`out_weather.c:343-387`). Notification `t_epoch` and "ago" (`svc.c:225`, `sys_notifs.c:347, 513, 949`), GPX `<time>` (`svc_track.c:160`), diag log event epoch (`diag_log.c:222, 453`), SD sensor CSV `utc_epoch` column from raw `time(NULL)` (`board.c:708-728`).

## How it runs

| Concern | Task / context | Priority / core | Cadence |
|---|---|---|---|
| Clock restore | `app_main` boot path (`nos_main.c:1922`) | UI task prio 1, core 0 | once, before radios |
| RTC write-back | `task-board-fs` | prio 1, unpinned | polled each 100 ms; retry 5 s; forced at shutdown |
| Calendar day / pedometer | `task-board-sense` | prio 2, unpinned | 20 ms loop; date recomputed 1 Hz |
| `act_tick`, `sports_tick` | UI task inside `svc_tick` | prio 1, core 0 | 1 Hz |
| `alarm_tick`, GB `apply_time` | UI task | prio 1, core 0 | every service tick (per frame; 1 Hz screen-off) |
| SNTP | lwIP tcpip + `ntp_synced` | prio 18 | hourly once Wi-Fi has an IP |
| BLE TIME char | NimBLE host task | prio 21, core 0 | on write |
| Sports journal writer | `task-sports-sd` | prio 1, unpinned | 1 Hz drain |
| Heart history/day | UI task | prio 1 | 60 s |

- Screen-off: `services_tick` (= `svc_tick` + `background_tick`, `nos/apps/registry.c:62-71`) runs at 1 Hz in `PWR_OFF` (`nos_main.c:2092-2095`) and via `nos_frame_run` at 1 Hz in AOD/sensor states (`nos_main.c:2108-2117`); alarms, `act_tick`, sports stamps, GB time frames keep working. `calendar_preferences()` runs every loop (`nos_main.c:1987`); sensor and fs tasks keep running. Everything stops only in shutdown/deep sleep after pedometer and RTC are flushed (`board.c:925-940`).
- Clock steps are instantaneous everywhere (`settimeofday`; SNTP immediate mode); nothing slews.
- Frame consistency: within a frame services share `s_frozen_dt`; outside frames (screen-off loop, other tasks) `hal_get_datetime` reads live.

## Data in / data out

| Source | Data | Entry point | RTC written? | Sets tz? |
|---|---|---|---|---|
| RTC chip | BCD UTC | `board_restore_clock` | no (only later on change) | no (tz from prefs) |
| Retained system clock | epoch | same, fallback branch | yes via `board_clock_changed` | no |
| Gadgetbridge `setTime;E.setTimeZone` | UTC secs + hours | `apply_time` -> `hal_set_datetime` | yes | yes, saved to prefs |
| SNTP | UTC | `ntp_synced` | yes, hourly | no |
| Legacy BLE TIME char | UTC [+tz min] | `time_access` | yes | `apply_tz` only; reverted next loop |
| Settings SET TIME | hour/minute | `hal_set_datetime_checked` | yes | no |
| Settings TZ picker | minutes east | `pick_commit` + `calendar_preferences` | no | yes |

| Consumer | Value read | Gate |
|---|---|---|
| Faces, home, Control Center, alarms, SET TIME | `hal_get_datetime` (local civil) | none (shows bogus 1970 time when unsynced) |
| Service day logic, weather fetch, notification ages, orrery UTC, Modules label | `hal_epoch_seconds` | >= 2024-01-01 |
| Pedometer day | `time()` -> localtime | >= 2020-01-01 and gate open |
| RTC write | `time(NULL)` | `s_system_trusted` (2000..2100) |

### Midnight / day-boundary consumers

| Consumer | Key / what happens at the boundary | Evidence |
|---|---|---|
| Pedometer `today` | local YYYYMMDD; on any change record zeroed, walk detector reset | `board.c:134-177`, `core/pedometer.c:9-14` |
| `nosact` week/hourly | local YYYYMMDD; rollover shifts week, wipes hourly; backward or > 7 days wipes the week | `svc.c:511-557` |
| Sports journal | `/sdcard/watch/sports/YYYYMMDD-events.spt`, `YYYYMMDD-HH-sensors.spt` (local date at record time; `00000000` unsynced) | `sports.c:215-262` |
| Energy app day pager | `sports_day(offset)` from live local date | `sports.c:102-109`, `fit_energy.c:34` |
| Heart min/max today | `day_key` change in `heart_tick`; `midnight` from epoch | `svc.c:586-640` |
| Alarms | per-minute key with `dt.day`; weekday bit; no stored date | `svc.c:826-851` |
| World clock | recomputed each frame from UTC + city rules | `time_worldclock.c:122-165` |
| Faces date line | `c->dt.weekday/day`, stateless | `faces.c:107-110` |
| Orrery face/card | JD from epoch; `tz_days`; ring cache keyed on calendar day | `face_orrery.c:1022-1044, 1141-1186`, `orrery_card.c:263-266` |
| Memo WAV names | UTC `M_YYYYMMDD_HHMMSS` (independent of tz) | `con_memo.c:445-448, 592-600` |
| Weather day labels | forecast location offset; "TODAY" vs `now` | `out_weather.c:343-387` |
| SD sensor CSV | raw `utc_epoch` per row; rotates by size | `board.c:708-728` |
| ext_hr history | epoch per sample; 24 h window by age | `ext_hr.c:216-227`, `svc.c:600-621` |
| Timer, stopwatch, snooze, workout durations | monotonic only | `svc.c:857-912`, `workout.c:197-205` |

### Behaviour on a time change

| Event | Pedometer | `nosact` | Sports journal | Other |
|---|---|---|---|---|
| Phone setTime, same local day | gate closed/reopened inside `hal_set_datetime_checked`; `activity.valid` blips 0 for a frame or two; count kept | kept; new steps go to the new hour bin | minute stamps step; order in file may step | alarm minute skipped if the step jumps over it; re-fires on a backward step |
| Forward jump crossing midnight (1..7 days) | zeroed, new day | week shifts by gap (first shift gets `max_steps`, rest zeros) | new date file | HR samples older than 24 h drop |
| Backward jump into an earlier date | zeroed (older day not restored) | `gap<0` -> 7: week and hourly wiped, `day` set to the earlier date | appends to the older date's file | HR last-reading frozen until the clock passes the old epoch |
| TZ change moving the local date (picker, GB zone, legacy BLE) | same as the matching jump (date recomputed within 1 s) | same | later records use the new local date, splitting the day | world clock and memo list follow tz; weather uses its own offset |
| TZ change within the same date | unaffected | new steps use the new hour | minute field jumps | alarms use new local time |
| Manual SET TIME across midnight | as forward/backward jump | as above | as above | seconds forced to 0 |
| First sync after unsynced boot | seed logic keeps only samples since the seed if the date differs from the saved day; same day keeps saved count | `s_as.day` (from NVS) vs new day decides rollover | pre-sync events stay in `00000000-*` | alarms may have fired on the bogus clock |

## Persistence

- RTC chip: UTC calendar; survives power loss while its cell lasts; OS flag marks it untrusted; not written while unsynced.
- System clock: retained across soft reset/deep sleep by the IDF RTC timer (assumed by `board.c:873-882` and the comment in `calendar_day`); lost at power cycle.
- NVS (`wos` namespace via `hal_store_*`): `pedometer` (<= 60 s old), `nosact` (<= 10 min old), `nosalarm`, `sportsfood`, `sportsmarks`; preferences `nosprefs` (incl. `tz_min`, default 330) are SD-authoritative at `/sdcard/watch/settings.0.bin|settings.1.bin` with NVS mirror (`hal_esp32.c:228-282`).
- SD: sports journals as above, memo WAVs (UTC names), sensor log `NNNN.csv`, `workouts.N.bin`, `imu/NNNN.csv`, `NNNN.gpx` (no date in name).
- RAM only: HR history ring (144), notifications and their `t_epoch`, RTC dirty/generation/trust flags, calendar tracker.

## Interactions

- [[Engine and scheduling]]: per-frame datetime latch, `nos_bg_tick`, screen-off 1 Hz service loop.
- [[Services hub]]: `apply_time`, alarms, activity, heart in `nos/svc.c`; weather fetch gated on epoch.
- [[Health and activity]]: pedometer day reset, `nosact`, hourly/week charts, heart history.
- [[Sports and workouts]]: date-keyed journal files, intake ledger, workout UTC stamps.
- [[Power and battery]]: shutdown flush order (pedometer + RTC flushed, `nosact` not); deep sleep wake = GPIO0 only.
- [[Storage and persistence]]: NVS keys, SD settings checkpoint (tz), sports/memo/CSV paths.
- [[Hardware and drivers]]: PCF85063 on the shared I2C bus, device lock/retry, `PIN_RTC_INT` unused.
- [[Connectivity and voice]]: Gadgetbridge `setTime`, SNTP on Wi-Fi up, legacy BLE TIME char, RESYNC.
- [[Shell and faces]]: face_ctx date, `fk_date`, no unsynced indicator, orrery face.
- [[Utility apps]]: alarms, world clock, recorder naming, timer/stopwatch immune to jumps.
- [[Settings and developer]]: SET TIME, TZ picker, Modules RTC detail, Diagnostics clock test.
- [[Calorie trace]]: SP_ACTIVITY (steps/kcal) is stamped with local day/minute and resets with the pedometer.
- [[Rendering pipeline]]: AOD date text changes only when a frame is rendered.
- [[Gaps]], [[OS overview]].

## Gaps

| # | Severity | Gap | Evidence | User impact |
|---|---|---|---|---|
| 1 | medium | Pedometer reset and `act_rollover` are unsynchronised: if `act_tick` sees the new date before the sensor task resets the pedometer (window up to 1 s, roughly half of midnights), `act_rollover(day, steps)` stores yesterday's total as the new day's `max_steps`/`last_steps`; `max_steps` only grows, so at the next rollover `week[5]` reports max(previous total, that day's total) | `svc.c:511-528, 546-548`, `board.c:154-176`, `core/pedometer.c:11-14` | 7-day bar for a day is wrong when it had fewer steps than the day before |
| 2 | medium | Any local-date change zeroes today's steps; a backward change also wipes the 6-day week and today's hourly (gap<0 -> 7) | `core/pedometer.c:11-14`, `svc.c:512-513`, `board.c:164-172` | Phone clock correction, travel timezone change or manual set across midnight destroys step history, no undo |
| 3 | medium | No unsynced indicator and no manual date entry: with an invalid RTC faces show 1970-01-01 at 05:30 (default tz 330) + uptime, alarms fire off that clock, SET TIME edits hour/minute only and is rejected when year < 2000; `HAL_FEAT_SYNC` is unused by NOS | `board.c:884-886`, `hal_esp32.c:116-129`, `set_settings.c:2343-2371`, `svc.c:826-851` | Dead RTC cell with no phone/Wi-Fi gives a plausible wrong time with nothing marking it and no way to set the date |
| 4 | medium | Pre-sync sports records go to `00000000-*.spt` and intake entries get `day=0`; the Energy app queries 19700101 while unsynced and later real dates, so they never appear in any day total | `sports.c:376, 115, 215-218, 317-318`, `fit_energy.c:34` | Food/water/events logged before first sync are invisible in per-day views |
| 5 | medium | Heart "last reading" ignores acquisitions older than the stored `bpm_last_epoch`; after a backward clock step new readings are dropped until the clock passes the old value | `svc.c:625-634, 600-612` | Heart widget stuck for as long as the backward step (e.g. 1 h) |
| 6 | low-medium | `nosact` (hourly bins, active minutes, week, `last_steps`) is checkpointed every 10 min and not flushed at shutdown/restart while `pedometer` (<= 60 s) and the RTC are; after reboot the next tick attributes `steps - last_steps` (up to 10 min of steps) to the boot hour | `svc.c:558`, `board.c:925-940`, `svc.c:540-547` | Up to 10 min of hourly/active-minute data lost or mis-binned per restart |
| 7 | low-medium | A failing RTC blocks shutdown: after a clock change the dirty flag persists if I2C writes fail, `rtc_maintain(1)` fails in the stop path and `board_prepare_sleep` fails (shutdown/restart cancelled) | `board.c:935, 1146-1154, 801-828`, `nos_main.c:808-811` | With an RTC I2C fault after a sync the watch cannot power off or restart cleanly |
| 8 | low | Four "valid clock" thresholds (2000 RTC/BLE/GB/hal_set; 2020 calendar/retained/workout/ext; 2024 `hal_epoch_seconds`): a clock dated 2020-2023 is trusted by the pedometer but "not synced" to UI/weather/orrery | `board_protocol.c:41-44`, `board.c:158, 876`, `hal_esp32.c:151`, `workout.c:209` | Inconsistent behaviour on a wrong-but-recent date |
| 9 | low | No source arbitration or hysteresis: each GB connect and hourly SNTP step overrides the clock, each GB zone overrides the user's tz; every change rewrites the RTC (STOP/restart resets the RTC sub-second phase); RESYNC does nothing for a GB-only phone | `svc.c:1000-1019`, `hal_esp32.c:1057-1068, 1835-1841`, `board.c:767-799`, `set_modules.c:1372-1374` | Clock jumps by phone skew; manual tz overwritten; RESYNC toasts without effect |
| 10 | low | Boot trusts the RTC over a retained system clock even if the RTC write was pending/failed (dirty flag is RAM-only); the RTC is never re-read to detect drift | `board.c:853-880, 54-72` | Rare backward step at reboot, possibly crossing midnight |
| 11 | low | Alarm detection is per-minute edge with no catch-up and no validity gate: a forward step over the alarm minute skips it, a backward step re-fires it, rebooting inside the alarm minute rings it (`s_alarm_key=-1`) | `svc.c:826-851, 90` | Missed or duplicate alarms around time changes; ring on the bogus pre-sync clock |
| 12 | low | Legacy BLE TIME char tz bytes change only `g_tz_offset_min`; `calendar_preferences` resets it to `nos_prefs.tz_min` next loop, so the tz is discarded; `apply_tz` does `setenv/tzset` on the NimBLE host task while other tasks call `localtime_r` | `hal_esp32.c:881-884, 822-830`, `nos_main.c:962-966` | Companion-pushed timezone has no lasting effect; theoretical TZ string race |
| 13 | low | Local tz is a fixed offset with no DST; DST changes need a new phone zone or manual change, while world-clock cities model DST | `hal_esp32.c:822-830`, `time_worldclock.c:100-120` | Local time off by 1 h after a DST change until next phone sync |
| 14 | low | Orrery unsynced fallback treats local time as UTC and leaves `tz_days` stale; a card opened unsynced never advances | `face_orrery.c:1141-1146, 1183-1186`, `orrery_card.c:263-266, 1931` | Wrong sky/lighting until synced |
| 15 | info | Date-keyed stores use mixed bases: sports journals by LOCAL date at write time, memo WAVs by UTC, workouts display UTC with `Z`, weather by the forecast location; the simulator applies tz by shifting UTC rather than the displayed local time (`sim/nos_hal_sim.c:186-190`) | `sports.c:376`, `con_memo.c:445-448`, `fit_workout.c:843`, `out_weather.c:343-387` | Per-store "day" can disagree near midnight or after a tz change |
