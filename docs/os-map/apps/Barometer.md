---
title: "Barometer"
id: app-barometer
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Barometer

Source: `nos/apps/out_baro.c` (722 lines). Sensor driver: `firmware/main/ext/ext_env.c` (227 lines). HAL contract: `nos/hal_ext.h:164-176`. Read-only source review. Nothing was built, flashed or run, and the serial port was not used. Evidence is `repo-relative/path:line`.

## Summary

Barometer is a three-page pressure, altitude and climate screen (`scr_baro`, `nos/apps/out_baro.c:557`) with a sea-level picker pushed from its ALTITUDE page (`scr_baro_cal`, `out_baro.c:716`). All readings come from one optional strap module, the BME280 (7Semi Nano), through the HAL getter `hal_env_get`. The `ext` task refreshes that cached sample every 2 s (`firmware/main/ext/ext_env.c:22`, `:123-176`). The app calls no service getter (`svc_*`): `out_baro.c` includes `svc.h` (`:27`) but calls nothing from it. Its only preference read is `nos_prefs.units_metric`. Its only persistent effect is the sea-level reference, written to NVS key `ext_sea` through `hal_env_set_sea_level`.

Status is working by source review. All three pages, the picker and the absent and reading states are implemented. The gaps are mostly missing features: no alerts when the screen is closed, a 24 h history that resets at every boot, no hot-plug, and partly ignored unit preferences (see Gaps). Not bench-tested.

Hardware: a BME280 at I2C 0x76 or 0x77 on the shared bus (SDA47 / SCL48). A BMP280 (chip ID 0x58) is accepted but reports no humidity (`ext_env.c:101-106`, `:158`; `nos/hal_ext.h:14`). The module is probed once at boot (`firmware/main/ext/ext_sys.c:169-183`). The optional GPS (u-blox M10 on UART1, IO7 / IO17, `nos/hal_ext.h:12`) is needed only by the Use GPS button; its driver and power states are in [[Connectivity and voice]]. Pressure history lives in PSRAM (`out_baro.c:219`, `firmware/main/hal_esp32.c:1595-1598`). Wiring and presence: [[Hardware and drivers]].

## Features

- **PRESSURE page** (`page_pressure`, `out_baro.c:375-432`).
  - Hero value in hPa to one decimal (`fmt1`, `:263`; `refresh_values`, `:265-275`). It updates at most once a second (`baro_tick`, `:348`), and the sensor produces a new sample every 2 s.
  - Trend word and signed 3 h change (`:382-398`). RISING above +1.0 hPa, FALLING below -1.0 hPa, otherwise STEADY. Until the history holds 19 samples the row reads "TREND IN 3 h" (`:252-253`).
  - 24 h column chart (`:407-431`). Each column is the mean of up to six 10-minute samples (`read_history`, `:228-235`). Columns are right-aligned, 6 px wide on a shared baseline at row 342 (`:415-423`). The scale spans at least 4 hPa (`:240-241`). The newest column uses the live reading and wears the red cap (`:246`, `:426-429`). Until two columns exist the chart reads "COLLECTING" (`:409-412`).
  - STORM line. When the 3 h change is below -3.0 hPa, a red dot and "[ STORM ]" fade in under the chart (`:258-259`, `:389-395`). The line is drawn only while this page is on screen.
- **ALTITUDE page** (`page_altitude`, `:434-450`). Integer metres from the sea-level reference (`:272`, `:439`), with the caption "SEA LEVEL xxxx.x hPa" (`:446`). "[ SAVED ]" shows for 1.5 s after a Set (`:442-444`). The Calibrate button (`:449`) is live only on this page (`:540-541`), and a tap pushes the picker (`:545-549`).
- **SEA LEVEL picker** (`scr_baro_cal`, `:559-716`).
  - Hero value in hPa, clamped to 870.0 - 1085.0 for the +/- buttons and the drag (`:560-561`, `:586`). It starts from the current reference, or from 1013.25 if none exists (`:596-597`).
  - +/- buttons. A tap steps 0.1 hPa. Holding repeats after 400 ms at 8 Hz, and after 2 s each step is 1 hPa (`:615-623`, `:675-676`).
  - Vertical drag. Every 8 px of travel is one detent. The step size follows finger speed: 0.1, 0.5 or 1.0 hPa (`:697-710`).
  - Live "ALTITUDE nnn m" line (`:600`, `:625`, `:643`).
  - Use GPS is live only with a 3D fix. It sets the reference from the GPS altitude (`:611`, `:646`, `:677-682`).
  - Set calls `hal_env_set_sea_level`, shows [ SAVED ], and pops back (`:683-692`). The back gesture or side button cancels without saving (`:663`, engine pop).
- **LOCAL page** (`page_local`, `:456-483`). Temperature with a degree sign, in C or F from `units_metric` (`:273-274`, `:463`). Humidity appears as 20 dots at 5 % each, with "HUMIDITY nn%" (`:475-481`). If the sensor reports no valid humidity, the page shows "HUMIDITY UNAVAILABLE" (`:470-472`). The caption "ON WRIST" appears in both cases (`:472`, `:482`).
- **Absent and reading states.** No module: "Not connected", "Add the climate sensor for pressure", tag "CLIMATE MODULE" (`:503-508`). Module present but no valid sample for more than 6 s: title plus "READING" (`:509-513`; `nos/hal_ext.h:172`; `ext_env.c:127-128`, `:185`).
- **Feedback and motion.** Three-page pager with a pixel spring (`pg_input` / `pg_tick`, `:59-102`). Haptic tick on page change (`:100`), click on tap (`:547`), heavy on refused GPS (`:678`), double on Set (`:690`). Digit morphs and entry builds (`:109-125`, `:340-368`).
- **Sim-only knob.** `NOS_STORM=1` forces a -3.6 hPa trend. It is compiled out on ESP (`out_baro.c:206-215`).

## Reads

Service getters: none. `out_baro.c` includes `svc.h` (`:27`) but calls no `svc_*` function.

Preferences (no `prefs_*` call):
- `nos_prefs.units_metric` at `out_baro.c:273` (temperature) and `:463` (unit letter). Field: `nos/prefs.h:28`.

HAL, environment (`nos/hal_ext.h:164-176`):
- `hal_ext_present()` tested for `EXT_ENV` on every tick (`out_baro.c:342`). The bit is set once, in `env_init` (`ext_env.c:120`).
- `hal_env_get()` every tick while the module is present (`out_baro.c:345`), and in the picker (`:595`, `:609`). It returns the cached sample and marks it invalid after `NOS_ENV_MAX_AGE_MS` = 6000 (`ext_env.c:178-189`).
- `hal_env_history()` for 144 pressure samples, every 5 s while valid (`out_baro.c:221`, `:347`).
- `hal_env_set_sea_level()` on Set (`out_baro.c:686`).

HAL, GPS (`nos/hal_ext.h` GPS block):
- `hal_gps_enable(1)` on picker entry (`out_baro.c:603`). `hal_gps_enable(0)` on leave (`:605`). `hal_gps_get()` every picker tick (`:611`).

HAL, memory: `hal_big_alloc()` for the 144-float history buffer (`out_baro.c:219`).

Diagnostics: `diag_action_request`, `diag_action_context`, `diag_log_name_id` and `diag_action_result` for the SET action in `DIAG_DOMAIN_SENSOR` (`out_baro.c:684-687`).

Engine: `nos_t`, `nos_enter_t` (`:289`), `nos_revealed` (`:279`), `nos_push` (`:549`), `nos_pop` (`:692`), `nos_feedback` (`:100`, `:547`, `:586-587`, `:678`, `:681`, `:690`), `nos_subview` (`:520`), `nos_ink_dim` (`:495`), `nos_mblur_*` (`:527-528`). The motion probes compile only under `GFX_COST_MODEL` (`:352-367`, `:629-631`).

Other readers of the same getters, not this app: the Diagnostics Climate self-test (`nos/apps/set_diag.c:326-334`), the Modules status (`nos/apps/set_modules.c:219`), the Weather strap line (`nos/apps/out_weather.c:630`). These sit in the [[Services hub]] and [[Hardware and drivers]] paths, not in this screen.

## Writes

App RAM only:
- Screen state `B` (`out_baro.c:149-170`), the reference `s_sea` (`:176`), and the picker state `K` (`:565-575`). `baro_enter` rebuilds the state on a fresh entry, but not when returning from the picker (`:277-293`).
- NVS key `ext_sea`, through `hal_env_set_sea_level` (`firmware/main/ext/ext_env.c:192-196`). The call also updates the driver's in-RAM reference `E->sea` and the app's `B.sea` (`out_baro.c:686-689`).
- GPS stream enable state (`hal_gps_enable`, `out_baro.c:603`, `:605`).
- One diagnostic SET record per Set. Its result is hard-coded to PENDING, because the HAL setter has no save ACK (`out_baro.c:684-687`).

Nothing is written to svc, prefs, SD or Gadgetbridge.

## Background behaviour

| Who | What it does | Period, priority, core | Evidence |
|---|---|---|---|
| `ext` task | `env_service()` runs the BME280 forced-mode conversion (trigger, 50 ms wait, read), updates the cached sample and the 144-slot history | Sensor period 2000 ms (`ext_env.c:22`). Task priority 0, no core affinity, 4 KB stack in PSRAM (`ext_sys.c:186-187`). Loop sleeps for the shortest module wake, capped at 50 ms (`ext_sys.c:144-167`) | `firmware/main/ext/ext_sys.c:144-187`; `ext_env.c:123-140` |
| Pressure history | One pressure sample appended to the 144-slot ring when the 10 min timer is due | 600 000 ms (`ext_env.c:24`, `:167-172`) | `firmware/main/ext/ext_env.c:167-172` |
| Sports sample | `sports_emit(SP_ENV, 5 values)` on every valid acquisition, about 1800 per hour | Every 2 s, same task | `firmware/main/ext/ext_env.c:174` |
| `sports_sd` writer | Drains the queue to `/sdcard/watch/sports/<day>-<hh>-sensors.spt`. Each drain opens, appends, fsyncs and closes the file. Also drains the diagnostics ring | Priority 1, no affinity, 4 KB PSRAM stack. Drain at least every 1000 ms (`nos/sports.c:282-293`, `:323`) | `nos/sports.c:215-218`, `:243-272`, `:282-293`, `:323` |
| Jobs row | `observe()` maps SP_ENV into the "Climate sensor" row (stale after 7000 ms, always sampled) | Per sample, fed through the sports observer | `nos/background.c:32`, `:97`, `:124`, `:160-164` |
| Screen tick | `baro_tick` runs once per UI frame while the screen is on top | UI frame loop | `nos/nos.c:2100-2110`; `out_baro.c:340-368` |
| Screen-off services | With the display off, `nos_bg_tick` runs `svc_tick` and `background_tick` once a second. The screen's own tick does not run | 1 Hz | `firmware/main/nos_main.c:2091-2094`; `nos/apps/registry.c:62-71` |
| GPS stream | Runs only while the picker is open. Auto-stops after 8 s without a `hal_gps_get` call | `NOS_EXT_IDLE_MS` = 8000 (`nos/hal_ext.h:35`) | `firmware/main/ext/ext_gps.c:267-281`, `:308-315` |
| Shutdown | `env_shutdown_poll` puts the sensor into sleep (F4 = 0) and clears the cached sample | Shutdown poll at 100 ms | `firmware/main/ext/ext_sys.c:92`; `ext_env.c:219-227` |

Screen off. The climate sensor is never idle-gated. `env_service` has no idle check (`ext_env.c:130-140`). Its neighbours do have one: the GPS stream, the light-gesture mode, the compass and the heart-rate stream all stop after `ext_idle` (`ext_gps.c:267-270`, `ext_light.c:180`, `ext_mag.c:195`, `ext_hr.c:238`). So the BME280 samples, fills the history and feeds the SD journal whenever the module is fitted and the watch is on, whether or not the app is open.

Auto-sleep. The default is `sensor_idle` = 1 (`nos/prefs.c:159`). That maps to PWR_SENSOR (`nos/power_policy.h:13-14`, `firmware/main/nos_main.c:2085`). `go_state` calls `nos_home_now()` for PWR_SENSOR and PWR_AOD (`nos_main.c:705`), and `nos_home_now` leaves every screen above Home (`nos/nos.c:429-439`). So by default the Barometer and any open picker are closed and their `leave` callbacks run. With both `sensor_idle` and AOD off, auto-sleep goes to PWR_OFF instead, and the stack is kept (`power_policy.h:14`, `nos_main.c:705` not reached).

User shutdown. `sports_quiesce(1)` suppresses non-essential records, and SP_ENV is not essential (`nos/sports.c:122-141`; `firmware/main/nos_main.c:759`).

Cost, from code only (not measured): about 1800 records of 64 bytes per hour (`nos/sports.h:13-18`), so about 115 KB of SD appends per hour, or about 2.8 MB per day. The sleep current quoted in the driver header and `docs/MODULES.md:168` ("well under 10 uA") is not verified here. Power budget context: [[Power and battery]].

Doc versus code. The header says live values "change at most once a second" (`out_baro.c:12-13`). The sensor cadence is 2 s, so the numbers change about every 2 s.

## Persistence

- **NVS `ext_sea`** (float, 4 bytes). Read once at boot in `env_init` (`ext_env.c:113`), and used only if it lies in 800 - 1200 hPa. Otherwise the default 1013.25 applies (`ext_env.c:111`). Written only by Set (`ext_env.c:196`), via `hal_store_save` (`firmware/main/hal_esp32.c:256-257`) and `hal_store_save_checked` (`:228-236`), which calls NVS directly for this key. Survives reboot and reflash. No SD copy. The save result is discarded (see Gaps).
- **RAM only.** The 144-sample pressure history (`ext_env.c:36-37`, `:199-208`) is cleared at every boot (`ext_env.c:12-13`; `docs/MODULES.md:167` agrees). The app's own state is RAM.
- **SD, indirect, not read back.** SP_ENV records go to the sports sensor journal `/sdcard/watch/sports/<day>-<hh>-sensors.spt` (`nos/sports.c:215-218`, `:243-272`). The record layout is `nos/sports.h:13-18`. No NOS screen reads them back. `sports_query` returns energy and battery bins only, not pressure (`nos/sports.h:24-27`, `:48`). SP_ENV appears only in `ext_env.c:174`, `nos/background.c:97` and `nos/sports.c:179`, and the sim-only emit at `sports.c:395`. The journal belongs to [[Sports and workouts]] and the card-side layout to [[Storage and persistence]].
- **Diagnostics.** The SET action record goes to the RAM diagnostics ring. The sports writer drains it to `/sdcard/watch/diagnostics` (`nos/sports.c:289`; `nos/diag_log.c:24`).
- **Preferences.** None written. Units are read only.

## Navigation

Opened from:
- Launcher app list, entry "Barometer" with `DOT_ICON_GAUGE`, `scr_baro` (`nos/apps/registry.c:28`). The registry has no hardware filter, so the tile appears even without a module. The screen then shows "Not connected".
- Not linked from Home, a widget, a face complication or Control Center. A search of `nos/` and `firmware/main/` finds `scr_baro` only in `registry.c:28`, `nos/apps/nos_apps.h:51`, the scroll snapshot at `firmware/main/nos_main.c:1070`, and the sim table.

Opens:
- SEA LEVEL picker (`scr_baro_cal`), pushed with `TX_PUSH` by the Calibrate tap (`out_baro.c:545-549`). Set pops it (`:692`). Back or the side button cancels.

Gestures and pages:
- `SCR_HDRAG` (`out_baro.c:557`; `nos/nos.h:60-61`). Horizontal drags page the screen. The back swipe starts only at the left edge.
- Pages: 0 PRESSURE, 1 ALTITUDE (the only page with Calibrate, `out_baro.c:540-541`), 2 LOCAL.
- Returning from the picker does not rebuild the screen (`baro_enter`, `:279-282`).

Related: [[Shell and faces]] for launcher and Home placement, [[Engine and scheduling]] for the tick and screen-off loop.

## Gaps

1. **medium. No alert outside the open screen.**
   - Evidence: the storm flag is computed in `read_history` (`nos/apps/out_baro.c:257-259`) and drawn only in `page_pressure` (`:386-395`). The file calls no notification or haptic API. Its only feedback is `nos_feedback` on taps, page changes and picker actions (`:100`, `:547`, `:586-587`, `:678`, `:681`, `:690`).
   - Detail: no notification, no haptic, and no background check exists for a pressure drop.
   - User impact: a rapid fall, which the STORM line is meant to flag, reaches the user only if the app is open.

2. **medium. History and trend reset at every reboot, although the data is on SD.**
   - Evidence: `firmware/main/ext/ext_env.c:12-13`, `:36`, `:199-208`; `nos/apps/out_baro.c:252-256`, `:409-410`. The same samples reach the SD journal every 2 s (`ext_env.c:174`; `nos/sports.c:215-218`). No pressure read-back exists (`nos/sports.h:24-27`).
   - Detail: the RAM ring is the only source the chart and trend use.
   - User impact: after boot the chart reads "COLLECTING" for about an hour, and the 3 h trend is blank for about 3 h. The data is already on the card.

3. **medium. Save feedback is unconditional.**
   - Evidence: `hal_env_set_sea_level` returns void (`nos/hal_ext.h:174`; `firmware/main/ext/ext_env.c:192-196`). `hal_store_save` discards the result of `hal_store_save_checked` (`firmware/main/hal_esp32.c:256-257`). The app shows [ SAVED ] and pops regardless (`nos/apps/out_baro.c:686-692`). The diagnostic result is hard-coded PENDING (`:687`).
   - Detail: an NVS write failure is never reported.
   - User impact: a failed save looks successful. After the next reboot the reference silently returns to 1013.25 and the altitude shifts.

4. **medium. No hot-plug, and the unplug path cannot run on device.**
   - Evidence: `EXT_ENV` is set only in `env_init` (`firmware/main/ext/ext_env.c:120`). Probing happens once, at boot (`firmware/main/ext/ext_sys.c:169-183`). Nothing clears the bit. The unplug and replug fade (`out_baro.c:295-319`, `:342`) therefore runs only in the simulator.
   - Detail: a module fitted after boot is never probed.
   - User impact: the screen says "Not connected" with the hint "Add the climate sensor" until the watch reboots.

5. **medium. Climate sampling and SD journaling run with the app closed and no idle gate.**
   - Evidence: `firmware/main/ext/ext_env.c:130-140` (no idle check). Contrast `ext_gps.c:267-270`, `ext_mag.c:195`, `ext_hr.c:238`. Each sample feeds the journal (`ext_env.c:174`; `nos/sports.c:243-272`).
   - Detail: a 2 s forced-mode conversion and about 115 KB of SD appends per hour, while the screen is off and the app is closed.
   - User impact: continuous draw from the strap and SD wear, for data the user may never open. Cost is unmeasured here.

6. **medium. Temperature is labelled "ON WRIST" with no skin-heat correction.**
   - Evidence: `nos/apps/out_baro.c:472`, `:482`, `:273-274`. The module notes in `docs/MODULES.md:170-171` say temperature "reads high on the wrist" and that skin heat biases it.
   - Detail: the value is shown as if it were ambient, with no offset and no caveat.
   - User impact: the displayed temperature is biased upward, and the user is not told.

7. **low. Unit preference applies only to temperature.**
   - Evidence: altitude is always metres (`out_baro.c:272`, `:440`, `:643`). Pressure is always hPa (`:271`, `:406`, `:446`). Only temperature follows `units_metric` (`:273`, `:463`).
   - Detail: no feet or inHg option exists.
   - User impact: a user who has chosen imperial units still sees metres and hPa.

8. **low. Altitude silently uses the standard atmosphere until calibrated.**
   - Evidence: default reference 1013.25 when `ext_sea` is missing (`firmware/main/ext/ext_env.c:111-113`). The picker also falls back to 1013.25 (`nos/apps/out_baro.c:596-597`). The ALTITUDE page has no "uncalibrated" flag (`:434-450`).
   - Detail: the altitude is only as good as the local sea-level pressure. A 1 hPa error gives about 8 m of altitude error.
   - User impact: the number looks authoritative even when the reference is only the default.

9. **low. The sensor's failure states show no reason.**
   - Evidence: the screen shows only "READING" when no valid sample exists (`nos/apps/out_baro.c:509-513`). The driver invalidates the sample after 6 s without a new one (`ext_env.c:127-128`, `:185`) or after more than five read errors (`:147`). The only detailed explanation is the Diagnostics self-test, "NO SAMPLE IN 3 s" (`nos/apps/set_diag.c:330`).
   - Detail: no reason is given on this screen.
   - User impact: a failing sensor looks like a slow start, indefinitely.

10. **low. Use GPS can store a reference outside the picker's own range.**
    - Evidence: the GPS path sets `K.v` without the 870 - 1085 clamp that the +/- path applies (`nos/apps/out_baro.c:679` versus `:586`). Set passes the value on, and the HAL accepts 800 - 1200 (`firmware/main/ext/ext_env.c:194`).
    - Detail: a GPS altitude error of about 600 m at sea level moves the estimate above 1085 hPa, by the formula at `out_baro.c:199-204`.
    - User impact: the displayed value and the +/- limits disagree, and the out-of-range value can be saved.

11. **low. GPS can stay off after the screen sleeps inside the picker.** Conditional on non-default settings.
    - Evidence: the picker enables the GPS once (`nos/apps/out_baro.c:603`). Its tick only reads (`:611`). The driver stops the stream after 8 s without a read (`firmware/main/ext/ext_gps.c:267-270`) and restarts it only when `enabled` is set again (`:308-315`). With the display off the picker's tick does not run (`firmware/main/nos_main.c:2091-2094`). Auto-sleep collapses the stack only for AOD or sensor-idle (`nos_main.c:705`; `nos/nos.c:429-439`). The default `sensor_idle` = 1 collapses it (`nos/prefs.c:159`). The bug therefore needs `sensor_idle` off and AOD off (`nos/prefs.c:144`).
    - Detail: after wake, the picker is still open, but its GPS stream is off and nothing re-enables it. Not bench-tested.
    - User impact: Use GPS stays dimmed until the picker is reopened.

12. **low. On a watch without GPS, every picker entry costs a probe and the button gives no reason.**
    - Evidence: `hal_gps_enable(1)` on entry (`nos/apps/out_baro.c:603`) starts the absent-GPS re-probe (`firmware/main/ext/ext_gps.c:294-296`). That probe is two rounds of three bauds, each with a 600 ms wake and a 1500 ms listen (`ext_gps.c:243`, `:256`, `:258`, `:28`), so about 12.6 s. Use GPS dims with no text (`out_baro.c:644-646`, `:677-678`).
    - Detail: the UART runs a probe on each entry. The button gives no explanation.
    - User impact: a silent dimmed button, and a wasted probe on every visit.

13. **low. The chart's time label does not match the data.**
    - Evidence: "-24H" is fixed at the left edge (`nos/apps/out_baro.c:430`). The bars are right-aligned to the data (`:249-250`), which covers only the time since boot (`:221`). No value axis or hour readout exists, and the chart takes no input (`baro_input`, `:534-555`).
    - Detail: after a few hours the label claims a 24 h history that is not there.
    - User impact: misleading axis. The user cannot read any hour's value.

14. **low. The STORM name overstates a fixed heuristic, and the docs disagree with the code.**
    - Evidence: the state is a fixed 3 h change of -3.0 hPa (`nos/apps/out_baro.c:258`). The label reads "[ STORM ]" in primary ink, with only the dot red (`:392-395`). `docs/polish/outconn_r2.md:129` describes "[ STORM WARNING ]" as red text. That is stale.
    - Detail: no forecast or storm model is involved.
    - User impact: a user may read it as a forecast.

15. **low. Missing features a user would expect from a modern pressure and altitude screen.**
    - Evidence: no ascent or descent totals or elevation history exists in `out_baro.c`. The only sea-level sources are manual entry and GPS (`:594-605`, `:677-682`). History is 24 h at most (`firmware/main/ext/ext_env.c:23-24`). No feet or inHg option exists (see item 7). No skin correction exists (see item 6). No alerts exist (see item 1).
    - Detail: the app is a reading and calibration screen, with no trends beyond 3 h and no recording for later review.
    - User impact: hikers and climbers get no ascent figures. Users cannot get a QNH automatically. Any ascent feature would belong to [[Health and activity]] or [[Sports and workouts]]. A grep of `nos/` finds no pressure or climate reader in the activity or sports screens today.

Dead buttons: none. The one button that can dim (Use GPS) is covered by items 11 and 12.
