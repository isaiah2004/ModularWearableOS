---
title: "Diagnostics"
id: app-diagnostics
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Diagnostics

## Summary

Diagnostics is the NOS self-test: a guided pass over the board parts and the strap modules, with the sensor checks running in the background, then a result list. It is one screen (`scr_diag`, name "DIAG", title "DIAGNOSTICS", `nos/apps/set_diag.c:1375`) plus a row page (`scr_diag_row`, name "DIAG_ROW", `set_diag.c:1431`). The launcher tile is "Diagnostics" with `DOT_ICON_CHECK` (`nos/apps/registry.c:35`).

- It is a test run, not a live readout. There are 15 parts (`enum` at `set_diag.c:49-50`, `D_N`). Each resolves to PASS, FAIL, ABSENT ("NO MODULE", not connected) or SKIP (`set_diag.c:80`).
- Six parts are board parts: Button, Touch, Motion (IMU), Clock (RTC slot), Battery, SD card (`D_BOARD_N` 6, `set_diag.c:51`). Nine are strap modules: Haptics, Microphone, Speaker, GPS, Cellular, Climate, Light, Compass, Heart rate (`set_diag.c:60-77`).
- Four parts are guided: Button, Touch, Haptics and Speaker are cards that wait for the person (`is_card`, `set_diag.c:124`). The other eleven are automatic reads of cached HAL getters.
- The file header describes probe, read and plausibility checks with real thresholds (`set_diag.c:1-24`). The code matches that description. The verdicts are only as good as the presence bits and the getter caches they read, and several of those are weaker than the header implies (see Gaps).
- Nothing is kept after the screen closes. Results live in RAM and are cleared at the next run (`set_diag.c:439-442`, `:1234`).
- Entry points: the launcher tile (`registry.c:35`) and Settings > Modules > RUN DIAGNOSTICS (`set_modules.c:343`, `:652`).
- Status: **partial**. The screen is complete and wired to the HAL. Two strap parts cannot show NO MODULE on the 1.43-B, the Clock card does not test the RTC chip, and a screen-off during a strap test can produce a false FAIL.

Related: [[Services hub]], [[Hardware and drivers]], [[Health and activity]], [[Power and battery]], [[Storage and persistence]], [[Connectivity and voice]], [[Sports and workouts]], [[Shell and faces]], [[Engine and scheduling]].

## Features

- **Guided cards** (`card_tick`, `set_diag.c:659-669`; `card_input`, `:738-794`; `draw_card`, `:823-878`).
  - Button: "Press the button", SKIP pill. Any press and release, or a long press, passes as seen by the OS input (`set_diag.c:742-748`). A double press is ignored and the card keeps waiting (`:744`).
  - Touch: "Tap the dots". Two targets at (150,150) and (316,380) (`set_diag.c:611`). A hit is within 44 px. The card fails only after three near misses with one consistent offset (`touch_down`, `:703-736`). A touch on the back-swipe edge (x < 40) is ignored (`:705`). The button skips the card after 0.5 s (`:753-757`).
  - Haptics: plays `HAPTIC_BUZZ` with strength raised to at least 80 (`card_play`, `:637-648`), then asks "Felt it?" with NO / YES (`:782-788`).
  - Speaker: plays `SFX_NOTIFY` with volume raised to at least 60, then asks "Heard it?" (`:644`, `:783-788`).
  - A card unanswered for 30 s resolves SKIP as "NO ANSWER" (`CARD_WAIT_S`, `set_diag.c:610`, `:664-668`).
- **Automatic tests** (`diag_step` dispatch, `set_diag.c:505-523`):
  - Motion: average |g| 0.80 to 1.20 at about 3 Hz samples, FAIL when frozen or gyro over 500 deg/s (`test_imu`, `:174-198`).
  - Clock: system clock must advance with uptime within 1.2 s, and the year must be 2020 or later (`test_rtc`, `:200-216`).
  - Battery: rail 3.00 to 4.40 V (5.20 V on USB), percent 0 to 100 (`test_batt`, `:218-233`).
  - SD card: mounted, size over 0, free not over size, no "FAIL"/"CANNOT" message (`test_sd`, `:241-256`). No card is ABSENT ("NO CARD"), not FAIL.
  - Microphone: at least 10 reads, mean 15 to 105 dB, not railed, level moved (`test_mic`, `:258-285`).
  - GPS: NMEA sentences counted in 10 s; a fix is not needed (`test_gps`, `:289-305`).
  - Cellular: modem answers AT within 25 s; SIM state, registration and RSSI shown (`test_lte`, `:307-322`).
  - Climate: temperature -20 to 60 C, humidity 0 to 100 %, pressure 300 to 1100 hPa (`test_env`, `:324-339`).
  - Light: lux and proximity plausibility only (`test_light`, `:341-363`; the code comment at `:359-361` says it makes no liveness claim).
  - Compass: field 20 to 70 uT and not frozen (`test_mag`, `:365-383`). Calibration state is shown, not checked.
  - Heart rate: IR trace must move and skin contact must be seen (`test_hr`, `:385-400`).
- **Summary hero** (`draw_summary`, `set_diag.c:962-1026`). While running: "TESTING", "n OF m" tested parts resolved, and the part name or "+k" for the parts still running. After the run: "x/y" PASSED (hero) with a red dot and "n FAILED" beside it, or "n SKIPPED" or "n NOT CONNECTED" in grey. One dot per part, in list order.
- **Result list** (`list_layout`, `set_diag.c:903-923`). While running or with no fails: Board group, then Strap modules group. With fails: a Failed group first, then Passed, Skipped and Not connected groups (`:907-915`).
- **Row page** (`row_draw`, `set_diag.c:1388-1429`). Reached by tapping a row (`:1193-1197`). Shows the verdict, the reason line for failures, up to two raw-reading lines (`rd`), and the threshold text (`DI[].check`, `:60-77`). An absent part shows the standard "Not connected" or "No card" page with the Modules copy (`:1394-1398`, `set_modules_absent_copy`, `nos/apps/set_modules.c:238-247`).
- **RE-RUN** pill after the run (`set_diag.c:1098`, `:1198-1205`). Runs every test and card again and re-reads presence (`run_start(2)`, `:443`).
- **Feedback**: `nos_feedback(FB_ERROR)` or `FB_SUCCESS` at the end of the run (`run_finish`, `:480`). Card results tick (`:654`).

## Reads

### Service getters (`svc_*`)

- `svc_track_active()` only, used in `power_down` to decide whether GPS may be switched off (`set_diag.c:146`). It returns `T.active` from the route recorder (`nos/svc_track.c:312`, set at `:177` and `:285`). It does not cover a Sports workout (see Gaps).
- No other `svc_*`, workout or sports getter is read by this file.

### Prefs (read-only)

- `nos_prefs.haptic_strength`, default 70 (`nos/prefs.h:10`, `nos/prefs.c:139`). Read at `set_diag.c:634`, `:640`, `:784`.
- `nos_prefs.volume`, default 60 (`nos/prefs.h:12`, `nos/prefs.c:141`). Read at `set_diag.c:635`, `:643`, `:785`.
- No `prefs_*` call and no NVS key is read.

### HAL getters (all read RAM caches; no bus call from the tick)

- `hal_imu_sample(&im, &ms)` returns the smoothed IMU copy, valid if under 150 ms old (`firmware/main/board.c:486-493`). The copy is a one-pole filter at about 3 Hz over a 500 Hz sensor (`board.c:291-313`, comment and `imu_take`).
- `hal_get_datetime()` and `hal_millis()`: the ESP system clock via `gettimeofday`, not the PCF85063 (`firmware/main/hal_esp32.c:111-113`, `:142-146`). See Gaps.
- `hal_power_get()`: `rail_mv`, `battery_percent`, `external_power`, `sample_valid`. Forced invalid when older than 3000 ms (`hal_esp32.c:378-387`). Sampled once per second from the sense task (`board.c:1021`, `hal_esp32.c:316`, `:367`). `hal_battery_mv()` and `hal_battery_percent()` are fallbacks (`set_diag.c:223-224`; `hal_esp32.c:389`, `:392`).
- `hal_storage_get()`: mounted, total and free bytes, message (`firmware/main/board.c:534-538`; written by `storage_status` at `:541-549`).
- `hal_mic_db()` and `hal_mic_wave(out[32])` (`firmware/main/ext/ext_audio.c:406-416`). Each call also stamps `mic_last_ms` and wakes the mic task if it is idle.
- `hal_gps_get()`: fix, UTC, satellites and the good-checksum `sentences` counter. Returns 0 without `EXT_GPS` (`firmware/main/ext/ext_gps.c:317-333`).
- `hal_lte_get()`: powered, SIM state, registration, RSSI (`firmware/main/ext/ext_lte.c:463-473`).
- `hal_env_get()`: temperature, humidity, pressure, valid within `NOS_ENV_MAX_AGE_MS` 6000 (`firmware/main/ext/ext_env.c:178-187`; `nos/hal_ext.h:172`).
- `hal_light_get()`: lux and proximity, valid within `NOS_LIGHT_MAX_AGE_MS` 2000 (`firmware/main/ext/ext_light.c:236-248`; `nos/hal_ext.h:184`).
- `hal_mag_get()`: field, heading, calibrated flag; invalid after 500 ms (`firmware/main/ext/ext_mag.c:217-227`).
- `hal_hr_get()`: IR trace (`wave[47]`), quality, on-wrist, bpm. Zeroes on-wrist, bpm and quality when the sample is over 2000 ms old (`firmware/main/ext/ext_hr.c:343-355`).
- `hal_button_down()`: GPIO0 level. Used only for the card's `held` flag (`set_diag.c:662`; `firmware/main/ext/ext_sys.c:202-204`). The button verdict comes from the OS input event instead.
- `hal_ext_present()`: the EXT_* presence bits, read once per run (`set_diag.c:443`; `firmware/main/ext/ext_sys.c:197-199`).
- Engine state: `nos_t`, `nos_frame`, `nos_revealed()`, `nos_leaving_covered()`, `nos_enter_t()`, `nos_touch_travel()`. `nos_keep_awake()` is a write (see Writes).

### Hardware it needs (1.43-B)

- Display and touch: FT3168 at `0x38` (`firmware/main/pins.h:52`). The Button card needs the side button.
- Side button: GPIO0 (BOOT), active low (`firmware/main/ext/ext_priv.h:44`; ISR `firmware/main/nos_main.c:480-492`).
- IMU: QMI8658 at `0x6b` (`pins.h:53`), sampled by the sense task (`board.c:1022-1024`).
- RTC: PCF85063 at `0x51` (`pins.h:54`). Touched at boot and on clock writes only (see Gaps).
- SD: one-bit SDMMC, CLK 41, CMD 39, D0 40 (`pins.h:57` and neighbours). `BOARD_HAS_SD` 1 (`pins.h:63`).
- Battery: ADC GPIO4, channel 3, divider 3 (`pins.h:65-66`). The B branch has no charge-status pin (`PIN_CHG_STAT` appears only in the C branch, `pins.h:33`).
- Strap (the code's assumed mapping, see Gaps): haptic motor on GPIO16 (`ext_priv.h:33`), I2S mic and amp on GPIO 1, 2, 3, 5, 6 (`ext_priv.h:34-38`), GPS UART on GPIO 7 and 17 (`ext_priv.h:39-40`), LTE UART on GPIO 43 and 44 with power key GPIO18 (`ext_priv.h:41-43`). Strap I2C at 400 kHz (`ext_priv.h:46`).

## Writes

### Hardware state (RAM or peripheral, not persisted)

- GPS: `hal_gps_enable(1)` at run start for the GPS test (`set_diag.c:165`, `:452`), and `hal_gps_enable(0)` when it resolves unless `svc_track_active()` (`:146`). GPS has one enable flag with no owners (`firmware/main/ext/ext_gps.c:308-315`).
- Heart rate: `hal_hr_enable(1)` at run start and `hal_hr_enable(0)` at resolve (`set_diag.c:147`, `:166`). This is the LEGACY owner bit (`firmware/main/ext/ext_hr.c:327-328`), so Sports and the Heart app keep their own owners.
- Cellular: `hal_lte_get()` at test start, then `hal_lte_enable(1)` only if the modem is unpowered (`set_diag.c:167-170`). It powers off again only if this run powered it (`:148`). The power-up is a PWRKEY sequence that takes seconds (`nos/hal_ext.h:154`).
- Haptics: `hal_haptic(HAPTIC_BUZZ)` (`set_diag.c:641`), `hal_haptic_stop()` on abort (`:494`), `hal_haptic_set_strength()` (`:634`, `:640`). Strength is RAM only, floored at 80 for the test and restored after each card (`card_restore`, `:632-636`).
- Speaker: `hal_sfx(SFX_NOTIFY)` (`set_diag.c:644`), `hal_audio_volume()` (`:635`, `:643`). RAM only.
- Microphone: polling `hal_mic_db` and `hal_mic_wave` keeps the mic running while the test is live (`firmware/main/ext/ext_audio.c:406-410`).
- Screen awake: `nos_keep_awake()` every frame while any sensor test is running (`set_diag.c:527`; `nos/nos.c:585`).
- Feedback and navigation: `nos_feedback()` on taps and at the end (`set_diag.c:717`, `:775`, `:1195`, `:1202`, `:480`, `:654`), and `nos_push(&scr_diag_row, TX_PUSH)` when a row is tapped (`:1197`).
- Log lines through `hal_log()`: `DIAG START` (`:445`), one `DIAG <NAME> PASS|FAIL|ABSENT <reason>; <values>` per part (`log_res`, `:127-142`), `DIAG DONE` (`:477-479`), `DIAG ABORTED` (`:496`). Destination: ESP_LOGI (serial) and a 32-entry RAM ring (`firmware/main/hal_esp32.c:444-452`, `:471-474`). Nothing under `nos/` or `firmware/main/` reads that ring (`hal_log_get`, `hal_esp32.c:465`).

### RAM state (static, lost on leave)

- `R[D_N]` results, `T` per-test scratch, `G` run state, `C` / `Cp` card state, `DL` list state (`set_diag.c:88`, `:91-102`, `:104-118`, `:623`, `:888-897`). `run_start` zeroes `R` and `T` (`:441-442`).

### Engine side effect

- Pushing a screen calls `call_enter`, which queues `DIAG_NAVIGATION` with the screen's name id (`nos/nos.c:199-202`). The record goes to the diagnostics ledger on SD at `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`). Diagnostics' own verdicts are not written there: `set_diag.c` makes no `diag_*` call.

### Not written

- No `prefs_*`, NVS, SD or settings write. The screen does not call `hal_mag_calibrate` or `hal_env_set_sea_level`, which store calibration in NVS.

## Background behaviour

- **No task or timer of its own.** Tests advance only in `diag_tick` (`set_diag.c:1238-1241`) through `diag_step` (`:500-530`), which the engine calls from `tick_screens` on the top screen (`nos/nos.c:2101-2109`). The engine frame loop runs in `app_main` (`firmware/main/nos_main.c:1899`, loop at `:1977`), at `NOS_FPS` 40 (`nos/render_config.h:8`). The frame delta is wall time, clamped to 100 ms (`nos/nos.c:2306-2307`).
- **Row page open.** `row_tick` calls `diag_step` (`set_diag.c:1387`), so tests keep going under the row page. `diag_leave` sees `nos_leaving_covered()` and only marks `G.covered` (`:1230-1233`).
- **Back or home.** `diag_leave` calls `run_abort` (`:488-497`). It powers down every running test, restores haptic and audio from prefs, stops the motor and logs `DIAG ABORTED`. `G.inited` is cleared, so the next open is a fresh run (`:1234`).
- **AOD or sensor idle.** `go_state` calls `nos_home_now`, which calls leave on the screens above home (`firmware/main/nos_main.c:705`; `nos/nos.c:429-441`). That reaches `diag_leave`, so the run is aborted.
- **Plain screen off (PWR_OFF).** Nothing calls leave. `go_state(PWR_OFF)` (`nos_main.c:697-699`) does not collapse the stack. This is reached from the power sheet (`nos_host_power(0)`, `nos_main.c:754`) or from idle when AOD and sensor-idle are both off (`nos/power_policy.h:13-14`). The screen-off loop runs only `nos_bg_tick` at 1 Hz (`nos_main.c:2091-2094`), so `diag_tick` stops. The run is frozen, not aborted. Its strap streams are not renewed and idle-stop after `NOS_EXT_IDLE_MS` 8000 (`nos/hal_ext.h:35`; `ext_gps.c:267-270`, `ext_hr.c:238`, `ext_lte.c:302`). On wake nothing re-enables them (see Gaps).
- **Keep-awake.** Idle sleep is held while any test is `ST_RUN` (`set_diag.c:527`; the sleep policy at `nos_main.c:2078-2086`). A card waiting for the person does not hold it. A manual sleep still works.
- **Strap and sense tasks that feed the data** (not Diagnostics code, but they make its numbers):
  - `ext` task, priority 0, unpinned, PSRAM stack, 4 KB (`firmware/main/ext/ext_sys.c:186`). Its loop runs the GPS, LTE, env, light, mag and HR services with a wait of at most 50 ms (`ext_sys.c:144-170`).
  - `exta` audio task, priority 3, unpinned, PSRAM stack (`firmware/main/ext/ext_audio.c:344`). `extf` file task, priority 0 (`:350`).
  - `board-sense` task, priority 2, unpinned, static 4 KB stack (`firmware/main/board.c:1036`). About 20 ms cycle with two extra IMU reads (`board.c:1022-1024`). Power sampled once per second (`board.c:1021`).
  - Services (`svc_tick`, `background_tick`) run once per second while the screen is off (`nos_main.c:2093-2094`; `nos/svc.c:1303`; `nos/background.c:160`). Diagnostics is not in that list.
- **After the run.** `run_finish` (`set_diag.c:465-485`) stops nothing new. Each test's streams are powered down when it resolves (`power_down`, `:144-149`), except GPS while a route track is recording (`:146`).

## Persistence

- **Results: none.** Verdicts, readings and the hero count are RAM only (`set_diag.c:88`, `:104-118`). They are cleared at the start of a run (`:441-442`) and on leave (`:1234`). There is no last-run timestamp: `t_done` is an engine-time value for animation only (`:84`, `:107`).
- **Serial log only.** `DIAG` lines go to the serial console (`hal_esp32.c:471-474`). The device does not store them.
- **Navigation record on SD.** Opening the screen queues a `DIAG_NAVIGATION` record in the diagnostics ledger (`nos/nos.c:199-202`; `nos/diag_log.c:24`). It holds the screen's name id and depth, not results.
- **No calibration or preference is changed.**

## Navigation

- **Opened from:**
  - Launcher app grid, "Diagnostics" (`nos/apps/registry.c:35`), opened with `nos_open` from its icon.
  - Settings > Modules: the Settings Modules row pushes `scr_modules` (`nos/apps/set_settings.c:1855`), whose list ends with RUN DIAGNOSTICS (`nos/apps/set_modules.c:343`). The button pushes `scr_diag` (`set_modules.c:652`).
  - Declared in `nos/apps/nos_apps.h:56`.
- **Opens:** `scr_diag_row` (`nos/apps/set_diag.c:1197`) when a row is tapped. Back from the row returns to the list. The row page has no buttons (`row_input` returns 0, `set_diag.c:1430`), so back is handled by the engine default.
- **Does not open** any other app. RE-RUN and the card buttons are internal.
- **Leaves** by back, home, AOD or sensor idle, which abort the run (see Background behaviour). A plain screen-off freezes the run without leaving.

## Gaps

1. **Screen-off during a strap test can produce a false FAIL.** Severity medium, plausible from source, not reproduced on hardware. A manual sleep during a running GPS, HR or Cellular test freezes the run (`set_diag.c:527`; `nos/power_policy.h:13-14`; `firmware/main/nos_main.c:754`, `:697-699`, `:2091-2094`). The strap streams idle-stop after 8 s without a getter (`ext_gps.c:267-270`; `ext_hr.c:238`; `ext_lte.c:302`; `nos/hal_ext.h:35`). Getter calls renew the timestamp but do not re-enable the stream. The only re-enable is on return from a row page (`set_diag.c:1215-1222`). On wake the GPS test can fail with "NO NMEA IN 10 s" (`:302`), HR with "NO IR SIGNAL" (`:398`), and Cellular with "NO AT REPLY IN 25 s" (`:315`). Each reads like a hardware fault. The user impact is a healthy part shown as failed.

2. **Speaker and Haptics can never show NO MODULE on the 1.43-B.** Severity medium. `hal_ext_init` runs `haptic_init` and `audio_init` whenever `EXT_BOARD_OK`, which is 1 on B (`firmware/main/ext/ext_sys.c:177-179`; `ext_priv.h:27-29`). `audio_init` sets `EXT_SPEAKER` with the comment "cannot be probed; default on" (`ext_audio.c:343`). `EXT_HAPTIC` is set whenever the LEDC driver configures (`ext_haptic.c:109`). B has no built-in audio (`firmware/main/pins.h:64`). The DI entries for these two have a non-zero `ext` bit (`set_diag.c:67`, `:69`), so they are never ABSENT. The Speaker card still asks "Heard it?", and "NO" becomes FAIL "NOT HEARD" (`set_diag.c:787-788`). Those two verdicts count toward the hero "x/y" total. Their result is the person's answer, not a check.

3. **The Clock test does not check the RTC chip.** Severity medium. `test_rtc` reads the ESP system clock (`set_diag.c:200-216`; `hal_esp32.c:142-146`, `gettimeofday`). The PCF85063 is read only at boot in `board_restore_clock` (`firmware/main/board.c:847-866`) and written only in `board_clock_changed` (`board.c:747`). The sense loop has no RTC read (`board.c:967-1025`). A PASS "ON TIME" can show with a dead chip if the clock was set by a phone sync. A FAIL "DATE NOT SET" (`set_diag.c:214`) does not separate "no time source" from "chip failed". The row copy "ADVANCES WITH UPTIME" (`set_diag.c:64`) is accurate about what is checked, but the card name suggests more.

4. **No result history, timestamp or export.** Severity medium. Results are RAM only (see Persistence). The serial `DIAG` lines and the 32-entry ring are not readable from the watch (`firmware/main/hal_esp32.c:444-452`, `:465`; no caller under `nos/` or `firmware/main/`). The SD ledger gets navigation records, not verdicts (`set_diag.c` has no `diag_*` call). A user who closes the screen cannot see what failed or when, and a repair person cannot read the last run from the device.

5. **Presence is read once per run; the probes are asynchronous.** Severity low. `run_start` takes `hal_ext_present()` once (`set_diag.c:443`). Strap probes run inside the task after boot (`ext_sys.c:184`, comment "UART probes run asynchronously inside the task"). The presence bits are only ever set; the only clear is on audio start failure (`ext_audio.c:347`). A module that answers after the first run stays "NO MODULE" until RE-RUN. A module unplugged after boot stays "present" and fails with "NO REPLY" (`set_diag.c:293`, `:310`, `:388`).

6. **A Diagnostics GPS verdict can switch off a Sports outdoor session's GPS.** Severity low, plausible. `power_down` calls `hal_gps_enable(0)` unless `svc_track_active()` (`set_diag.c:146`). `svc_track_active` covers the route recorder (`nos/svc_track.c:312`), not `nos/workout.c`. Workout enables GPS with no owner bits (`ext_gps.c:308-315`) and re-asserts it per sample (`nos/workout.c:389`, called at `:464`, `:810`). The outage length is not measured here. Heart rate avoids this with owner bits (`ext_hr.c:327-339`).

7. **The Microphone "present" test is a signal-swing heuristic.** Severity low, unverified. `mic_ok` is set when the I2S read swings more than 2 counts on either channel (`ext_audio.c:223-230`). B has no mic (`pins.h:64`). A floating or noisy DIN could set `EXT_MIC`, and then the mic card could PASS on noise (`set_diag.c:272-284`).

8. **The Touch verdict covers two points.** Severity low. Only the two targets at `set_diag.c:611` are tested, and only the two dots are drawn. A dead region elsewhere on the panel passes. The back-swipe edge (x < 40) is ignored (`:705`).

9. **No single-part re-run.** Severity low. The row page has no action (`set_diag.c:1388-1431`). RE-RUN repeats every card and test (`:1198-1205`), so a fix for one part means a full pass.

10. **Motion has no keep-still instruction.** Severity low, plausible. The verdict averages about one second of samples and fails if the gyro passes 500 deg/s (`set_diag.c:174-198`, FAIL at `:196`). The card hints do not ask the person to keep still (`:828-831`).

11. **Strap pin map is asserted in code but not verified in the project record.** Severity medium for the nine strap rows. `ext_priv.h:24` says the strap is wired to the 1.43-B back headers, and `docs/strap_layout.html` is cited (`ext_priv.h:32`). `CLAUDE.md` says the B external strap "needs its own verified mapping". Nothing in this file can confirm the strap pins on a bench.

12. **Opening the screen spends power before the rows are read.** Severity low. Every sensor test starts at run start, including GPS (`set_diag.c:446-453`, `:165`), the HR sensor (`:166`) and the LTE modem power-up (`:167-170`). LTE can stay powered for up to 25 s and HR for its test. This is a cost, not a fault.

13. **Missing features a user would expect from a modern watch's self-test:**
   - Last-run time, result history and a copyable report (Persistence).
   - A single-part re-run (gap 9).
   - Battery health: the Battery card checks only rail range and percent range (`set_diag.c:225-232`). There is no capacity or trend, and no charge check, because the B branch has no charge-status pin (`pins.h`, B branch).
   - Motion and haptic guidance as in gap 10.
   - Results on the phone or in Health screens. None exists in this file.
