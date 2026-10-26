---
title: "Heart rate"
id: app-heart-rate
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Heart rate

## Summary

All paths are repo-relative. Everything below was read from source on 2026-10-23. Nothing was built, flashed or run on hardware, so timing and power figures are code-derived and the state-machine findings are inferred from code paths, not bench-tested.

- **Screen** `scr_heart` (`nos/apps/fit_heart.c:938`) and **home widget** `wg_heart` (`nos/apps/fit_heart.c:1025`). Launcher entry: `nos/apps/registry.c:18`.
- **Strap only.** The app needs the MAX30102 pulse-oximeter strap at I2C 0x57. It has no other heart source: no wrist sensor, no phone or BLE HR input.
- **Opening the app** takes the sensor stream (`hal_hr_request(HAL_HR_OWNER_HEART,1)`, `fit_heart.c:537`). **Leaving** releases it (`fit_heart.c:553`).
- **Nothing is persisted by the app.** The 24 h history and the last reading live in RAM and are gone after a reboot (`firmware/main/ext/ext_hr.c:61,317`, `nos/svc.c:72`).
- **A 24 h history is built anyway.** The driver takes one background measurement every 10 minutes, up to 40 s, whether or not the app is open (`firmware/main/ext/ext_hr.c:35,247,322`).
- **No settings, buttons or sub-screens.** Page 0 is the live measurement. Page 1 is the 24 h view. The only input is the vertical pager (`fit_heart.c:936`, `fit_heart.c:130-169`).
- **Status: partial.** The core measure, lock, trace and history paths are implemented. The gaps below are real contract mismatches or stuck states.

## Features

Page 0, live measurement (`fit_heart.c:908-934` draws it):
- **ABSENT**: "Not connected", "Add the strap to measure BPM and SpO2", with a module tag (`fit_heart.c:771-780`).
- **NOCONTACT**: "Place the watch snugly" and "NO CONTACT", with a watch-settling dot illustration (`fit_heart.c:782-786`, `fit_heart.c:464-499`).
- **MEASURING**: a live PPG dot trace, 4 s window, 20 Hz playhead (`fit_heart.c:410-461`, `fit_heart.c:375-407`). A white heart pulses on each detected beat (`fit_heart.c:694-696`). Label "[ MEASURING ]".
- **LOCKED**: BPM as a large dot-matrix number that morphs on change (`fit_heart.c:808-815`), a red heart (`fit_heart.c:796`), SpO2 as "NN%" when present (`fit_heart.c:819-824`), and a signal meter. The trace shrinks below (`fit_heart.c:690-691`).
- **Lock rule** (`fit_heart.c:631-656`): on wrist, bpm 35-230, quality 0.6-1.0 (`LOCK_Q`, `fit_heart.c:41`), at least 0.5 s of good signal and 2 s of skin contact.
- **Haptic** `HAPTIC_HEARTBEAT` once per lock, only if `nos_prefs.haptics` (`fit_heart.c:652-653`).
- **Signal meter**: five 10 x 10 squares; "SIGNAL" or "NO SIGNAL" (`fit_heart.c:753-769`).

Page 1, LAST 24 H (`fit_heart.c:865-906`):
- MIN / AVG / MAX of the rolling 24 h history (`fit_heart.c:869-871`).
- 48 x 8 dot-column chart of 144 ten-minute bins; newest column white (`fit_heart.c:872-883`).
- Time axis 00 / 06 / 12 / 18 relative to now, with NOW at the right (`fit_heart.c:884-895`).
- Footer: "LAST nn BPM" (stored) or "NOW nn BPM" (live, while locked), crossfaded (`fit_heart.c:896-905`).
- Empty history shows "NO READINGS YET" (`fit_heart.c:882`).

Home widget, on home pager page 2 (`nos/apps/sys_home.c:203`; `fit_heart.c:955-1021`):
- Last BPM in large dots; age label "JUST NOW" / "n MIN AGO" / "n H AGO" / "n D AGO" / "TIME CHANGED" / "LAST READING" (`fit_heart.c:955-970`).
- 24-column mini history, 5 rows.
- "TAP TO MEASURE" shown only when the module is present; "NO HR SENSOR" otherwise (`fit_heart.c:1010-1020`).
- Tap opens the app (`fit_heart.c:1023`).

Other entry points:
- Split watch face, row 0, opens the app (`nos/faces/face_split.c:225`).
- Serial bench open-by-name table (`firmware/main/nos_main.c:931`).
- Settings > Modules > Heart rate detail toggles the LEGACY owner stream (`nos/apps/set_modules.c:83,920`). That is a module test, not this screen.
- Developer diagnostic "HEART RATE" (`nos/apps/set_diag.c:76,147,166,388,1219`).

## Reads

Services, from `svc_heart()` (`nos/svc.h:105-116`):
- `svc_heart_t` fields: `bpm_last`, `bpm_last_epoch`, `resting`, `min_today`, `max_today`, `hist[144]`, `nhist`, `spo2_last` (`fit_heart.c:198-205`).
- Read by `hdata_read()` at enter (`fit_heart.c:543`), on first reveal of page 1 (`fit_heart.c:706`) and by the widget (`fit_heart.c:957`). Page 1 is not re-read while open.
- `svc_heart_sample_epoch(sample_ms)` gives the wall-clock epoch of a sample (`fit_heart.c:662`; `nos/svc.c:619-623`).

HAL and drivers:
- `hal_hr_get(&hal_hr)` (`fit_heart.c:512,622`). Returns `on_wrist`, `bpm`, `spo2`, `quality`, `wave[48]`, `sample_ms` (`nos/hal_ext.h:202-209`). Called every app tick while `H.present`.
- `hal_hr_get` returns 1 whenever the driver object exists, even if the strap was unplugged. It zeroes the fields only when `sample_ms` is older than 2000 ms (`firmware/main/ext/ext_hr.c:343-356`).
- `hal_ext_present() & EXT_HR` (bit 1 << 8, `nos/hal_ext.h:60`) at enter, on strap return and in the widget (`fit_heart.c:512,536,608,958`). The bit is set once at boot (`ext_hr.c:324`) and never cleared at runtime.
- `hal_get_datetime()` for the local hour on page 1 (`fit_heart.c:602`). `hal_millis()` (`fit_heart.c:521,607`). `hal_epoch_seconds()` for the widget age (`fit_heart.c:962`).

Prefs and navigation:
- `nos_prefs.haptics` (`fit_heart.c:653`; default 1 at `nos/prefs.c:138`).
- `nos_revealed()`, `nos_leaving_covered()`, `nos_entered_by_finger()`, `nos_enter_t()`, `nos_t` (`fit_heart.c:506,528,552`).

Not read: no NVS keys, no SD files, no `hal_hr_history()` (only the service reads the history, `nos/svc.c:591-592`).

## Writes

Hardware and driver:
- `hal_hr_request(HAL_HR_OWNER_HEART, 1|0)` at enter, on strap return and at leave (`fit_heart.c:514,537,553,612`). This sets an owner bit and `enabled` (`ext_hr.c:330-341`). The driver then starts the MAX30102, or shuts it down (SHDN) when no owner remains (`ext_hr.c:230-258`).
- `hal_haptic(HAPTIC_HEARTBEAT)` on lock (`fit_heart.c:653`). No-op if the strap haptic driver is not up (`firmware/main/ext/ext_haptic.c:114-116`).
- `nos_keep_awake()` (`fit_heart.c:671`) sets `s_last_input_ms` (`nos/nos.c:585`). This is the display-sleep timer's input.

Services:
- `svc_heart_record_at(bpm, spo2, epoch)` on every fresh locked sample (`fit_heart.c:662-663`) and again on leave (`fit_heart.c:554`). It writes `bpm_last`, `bpm_last_epoch`, `min_today`, `max_today`, `spo2_last` and the day min/max (`nos/svc.c:625-643`). RAM only.
- It does not add to `hist[]`. The 24 h history is rebuilt every 60 s only from HAL samples (`nos/svc.c:586-617`).

Side effect on SD (not app-owned, but caused by the sensor running):
- Every 100 Hz sample emits `SP_PPG` (`firmware/main/ext/ext_hr.c:284`). Every FIFO burst emits `SP_HEART` (`ext_hr.c:296`).
- Both are raw types (`nos/sports.c:56`). They are queued with no Sports-session check (`nos/sports.c:134-142`) and drained to `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt` (`nos/sports.c:25,217`).
- Each record is 64 bytes (`nos/sports.h:13-19`). At 100 Hz PPG that is about 6.4 KB/s, or roughly 23 MB per hour, while the sensor streams.

Local state: `H` (`fit_heart.c:268-325`) and `WH` (`fit_heart.c:944-953`) are RAM only and reset on enter (`fit_heart.c:520-545`).

## Background behaviour

What runs, and who runs it:

| Runner | Where | Period | When it runs |
|---|---|---|---|
| `ext` task, priority 0, unpinned, 4 KB stack in PSRAM | `firmware/main/ext/ext_sys.c:186`; loop at `ext_sys.c:144-165` | Loop wait at most 50 ms (`ext_sys.c:148`). FIFO poll every 120 ms while running (`ext_hr.c:257`). Idle returns 1000 ms (`ext_hr.c:255`) | Always, since boot |
| Background HR measurement | `ext_hr.c:247-248,301-304` | First at boot + 2 min (`ext_hr.c:322`), then every 10 min (`ext_hr.c:34`). Each run is at most 40 s (`ext_hr.c:35`). It ends early after 8 s of good quality (>= 0.6) or 5 s off wrist | Always: screen off, app closed |
| UI `svc_tick` / `heart_tick` | `nos/svc.c:1303`, `svc.c:1322` (60 s), boot `svc.c:1295` | 60 s | Screen on, or screen off. The screen-off loop calls `nos_bg_tick` at 1 Hz (`firmware/main/nos_main.c:2091-2094`), which runs `services_tick` (`nos/apps/registry.c:64-65`) |
| Background "Heart sampling" job | `nos/background.c:30`, observer `background.c:100-105` | Fed by `SP_HEART` | Diagnostic status only. Not used by the app |
| Sports writer task | `nos/sports.c:282-292` | About 1 s, or sooner when records are queued | Whenever records are queued, including all HR and PPG samples |
| App `heart_tick` | `fit_heart.c:594-750` | Per frame | Only while this screen is top in a live frame. Not run in PWR_OFF (`nos_main.c:2091-2094`) |

Sensor lease:
- The sensor runs while an owner is set and some getter has renewed the lease within `NOS_EXT_IDLE_MS` = 8 s (`ext_hr.c:238`; `nos/hal_ext.h:27,35`).
- Leaving releases the owner (`fit_heart.c:553`). A cover (sheet, notification) keeps the stream (`fit_heart.c:548-552`), but the lease lapses after 8 s without a tick.

Screen-timeout interplay (default prefs):
- MEASURING calls `nos_keep_awake()` every frame, with no timeout (`fit_heart.c:671`). The screen never sleeps while MEASURING.
- LOCKED stops keeping the screen awake after 20 s (`fit_heart.c:671`).
- Default `sleep_s` = 15 (`nos/prefs.c:149`) and default `sensor_idle` = 1 (`nos/prefs.c:159`). Timeout then goes to PWR_SENSOR, and `go_state()` calls `nos_home_now()` (`firmware/main/nos_main.c:2085,705`; `nos/nos.c:429-439`). That pops this screen with `leave()` and releases the sensor.
- Inferred result: a locked reading left untouched returns to home about 35 s after the lock (20 s + 15 s).

## Persistence

- **App**: none. No prefs keys, no NVS, no SD file written by the screen itself. `H` and `WH` are reset on every enter (`fit_heart.c:520-545`).
- **Driver**: 144-entry history of `{sample_ms, epoch, bpm}` in PSRAM (`firmware/main/ext/ext_hr.c:61,317`). Not in NVS or SD. Lost on reboot.
- **Service**: `s_heart` and the day min/max are static RAM (`nos/svc.c:72,98`). Nothing about HR survives a reboot. `s_heart` is referenced only at `svc.c:581-643,1145,1295`.
- **SD**: only raw sensor journal records, `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt` (`nos/sports.c:25,217`). These are per-sample PPG and HR records, not the 24 h history, and they are written even outside Sports sessions (see Writes).
- **Settings**: `haptics` only (read, not written, by this screen).

Consistent with [[Health and activity]] ("Nothing about HR survives a reboot").

## Navigation

- **Opened from**: home pager widget page 2 (`nos/apps/sys_home.c:203`; widget tap `fit_heart.c:1023` uses `nos_open`). App launcher "Heart rate" (`nos/apps/registry.c:18`). Split face row 0 (`nos/faces/face_split.c:225`). Serial bench open-by-name (`firmware/main/nos_main.c:931`).
- **Opens**: nothing. The screen does not push another screen.
- **Leaves**: back gesture, the stack collapse on AOD / sensor-idle (`nos/nos.c:429-439`), or a cover.
- **Internal**: vertical pager between page 0 and page 1 (`fit_heart.c:130-169`). Page 1 builds its columns once, on first reveal (`fit_heart.c:706-712`).
- **No dead buttons**: the screen has no touch targets other than the pager.

## Gaps

Ordered by severity. Evidence is repo-relative path:line.

1. **[high] Stuck in MEASURING after a PWR_OFF sleep.** While the app is open, a PWR_OFF sleep does not pop the screen, unlike PWR_AOD and PWR_SENSOR (`firmware/main/nos_main.c:691-707`, the `nos_home_now()` call only on line 705). The screen tick is not run in PWR_OFF (`nos_main.c:2091-2094`), so the lease lapses after 8 s and the sensor is shut down (`ext_hr.c:238`). On wake, `heart_tick` only calls `hal_hr_get`, which renews the time but not `enabled` (`ext_hr.c:343-356`). Nothing re-requests `HAL_HR_OWNER_HEART` on wake: the only calls are at enter, reveal and strap return (`fit_heart.c:514,537,612`). The screen then shows MEASURING with no data until the user leaves and re-enters. PWR_OFF is reached when the battery is low (`nos/power_policy.h`, `power_idle_target`) or when the user sets "IDLE 80MHz OFF" with AOD off (`nos/apps/fit_energy.c:155,247`; defaults `prefs.c:144,159`). Inferred from code; not bench-tested.
2. **[medium] A locked reading closes to home.** The keep-awake stops after 20 s of LOCKED (`fit_heart.c:671`). With default `sleep_s` 15 (`prefs.c:149`) and `sensor_idle` 1 (`prefs.c:159`), the timeout pops the screen to home (`nos_main.c:2085,705`; `nos.c:429-439`). The 20 s cap has no comment. Impact: the user loses the reading after about 35 s if they do not touch the watch.
3. **[medium] No sensor-lost state; MEASURING forever, display never sleeps.** The strap presence bit is never cleared at runtime (`ext_hr.c:324`). `hal_hr_get` keeps returning 1 once the driver exists (`ext_hr.c:356`). A silent sensor, or an unplugged strap, maps to MEASURING (`fit_heart.c:622-623,631-636`). MEASURING keeps the display awake (`fit_heart.c:671`). Impact: no message, and the screen never times out, which drains the battery.
4. **[medium] Live readings never reach the 24 h chart or its MIN / AVG / MAX.** The history is built only from HAL samples (`nos/svc.c:602-612`). `svc_heart_record_at` does not touch `hist[]` (`svc.c:625-643`). The HAL history gets one point per 10 minutes, from the background run or, if the stream is live at that moment, from `H->cur.bpm` (`ext_hr.c:243,248,304`). Impact: the chart shows at most one point per 10 min and MIN / AVG / MAX reflect background samples, not the user's locked readings.
5. **[medium] Empty history shows contradictory numbers.** `hdata_read` takes `min_today` and `max_today` from the service (`fit_heart.c:201-203`). They are overwritten only when the history has data (`fit_heart.c:217-221`). With no history bins but a live locked reading, MIN and MAX show that reading, AVG shows "--" and the chart says "NO READINGS YET" (`fit_heart.c:869-882`). Impact: after a cold boot, the page disagrees with itself.
6. **[medium] No persistence across reboot.** The driver history is PSRAM only (`ext_hr.c:61,317`) and the service state is RAM only (`svc.c:72`). Impact: the 24 h history and the last reading reset on every restart, and the widget shows "NO READINGS YET" until the next 10-minute sample. The 1.43-B has SD, but no HR store uses it.
7. **[medium] Raw PPG and HR go to SD whenever the sensor runs, not only in Sports.** `SP_PPG` at 100 Hz and `SP_HEART` are queued without a session check (`ext_hr.c:284,296`; `nos/sports.c:134-142`) and written to hourly SD files (`sports.c:217,56`). Each record is 64 bytes (`sports.h:13-19`), about 6.4 KB/s or 23 MB/h while streaming. Impact: SD writes and power that the user cannot see from this screen. The screen does not disclose it.
8. **[medium] Stale BPM presented as live.** Gadgetbridge realtime `hrm` sends `s_heart.bpm_last` with no age check (`nos/svc.c:1145`). The home face does the same (`nos/apps/sys_home.c:473`). `bpm_last` never expires (`svc.c:602-605,629-632`). Impact: a reading from the last background run, or from hours earlier, is shown or sent as current.
9. **[low] Resting HR promised but not shown; derived path is dead.** The header says page 1 shows "today's resting / min / max" (`fit_heart.c:11`). `draw_history` shows only MIN / AVG / MAX (`fit_heart.c:869-871`). The service computes `resting` as the minimum over all bins (`svc.c:613-615`), so the "mean of the lowest quarter" fallback (`fit_heart.c:222-229`) never runs when there is any history.
10. **[low] Page 1 does not refresh while open.** It is read once at enter and once on first reveal (`fit_heart.c:543,706`). A 10-minute background sample therefore does not appear until the app is re-entered.
11. **[low] SpO2 shown with no caveat.** The driver says it is "NOT a medical measurement" (`ext_hr.c:17-20`). The screen shows "NN%" with no qualifier (`fit_heart.c:819-824`).
12. **[low] Live readings can be silently dropped without a clock.** `svc_heart_sample_epoch` returns 0 when the clock is unset (`svc.c:619-623`). `svc_heart_record_at` then discards any reading once a dated reading exists (`svc.c:629-631`), and the app passes the 0 through (`fit_heart.c:662-663`).
13. **[low] Missing features expected on a modern HR app.** None found by grep: no HR high/low alerts (no bpm code in any alarm or notification file), no history beyond 24 h, no history sync to the phone (only realtime `hrm`, `svc.c:1141-1146`), no BLE HR-strap input, no HRV, no zones. The app is wrist-only and strap-only. When the strap is absent the screen offers no alternative (`fit_heart.c:771-780`).

Doc versus code, for the record:
- `fit_heart.c:11` says page 1 shows resting. It does not (gap 9).
- `fit_heart.c:223` describes resting as the mean of the lowest quarter. The service computes a minimum, so the path is dead (gap 9).
- `firmware/main/ext/ext_hr.c:23-24` header: the 10-minute cadence, the 40 s cap and the 8 s early exit match the code.
- `docs/os-map/systems/Health and activity.md` says the lock rule and "does not add to the 24 h history" match the code.

## Related system notes

- [[Services hub]]: `svc_heart()`, `svc_heart_record_at()`, `heart_tick` at 60 s.
- [[Health and activity]]: the 24 h history and the background 10-minute sample.
- [[Sports and workouts]]: `SP_PPG` and `SP_HEART` journaling, and the workout owner on the same sensor.
- [[Power and battery]]: keep-awake, the sleep policy and PWR_OFF / PWR_SENSOR.
- [[Storage and persistence]]: RAM-only HR state; SD sensor journal.
- [[Hardware and drivers]]: MAX30102 at 0x57, strap wiring, the ext task.
- [[Connectivity and voice]]: Gadgetbridge realtime `hrm`.
- [[Shell and faces]]: home pager widget and the split-face complication.
- [[Engine and scheduling]]: screen tick, `nos_bg_tick` at 1 Hz with the screen off, the ext task loop.
