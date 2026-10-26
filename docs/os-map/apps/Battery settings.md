---
title: "Battery settings"
id: sys-battery
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Battery settings

Source: `nos/apps/set_battery.c` (95 lines; screen `scr_battery` at `:94`, title "BATTERY" at `:69`). Target: 1.43-B build. Read-only source review: nothing was built, flashed or run, and the serial port and watch were not used. Evidence is `repo-relative/path:line`. The data comes from the board sampler in `firmware/main/hal_esp32.c`, not from this file.

## Summary

Battery settings is a read-only page with no controls. It shows the level, the power source, the system rail voltage and how fresh the sample is.

The level is shown only when the firmware marks the sample as valid. That happens only while the watch runs on its cells and the rail has been steady for about 10 s (`nos/apps/set_battery.c:45-47`, `firmware/main/hal_esp32.c:355-361`). Otherwise the hero reads `--`.

The board has no fuel gauge. The percentage is an estimate from the system rail (GPIO4 ADC through a divide-by-3, `firmware/main/pins.h:65-66`) mapped through a LiPo voltage table (`firmware/main/hal_esp32.c:300-308`). The state line separates "USB host seen", "on battery" and "unknown". It does not report charging, full or not charging.

Status: **partial**. This is a source-level judgement, not a bench result.

Related: [[Power and battery]], [[Services hub]], [[Hardware and drivers]], [[Storage and persistence]], [[Shell and faces]], [[Engine and scheduling]], [[Connectivity and voice]], [[Sports and workouts]], [[Health and activity]], [[Settings]], [[Energy]].

## Features

- **Title** "BATTERY" (`set_battery.c:69`).
- **Hero**: `NN%` in dot numerals, revealed over 0.40 s (`:71-73`), only when valid. Otherwise `--` (`:46-47`; `UI_NONE` is `"--"`, `nos/ui.h:187`).
- **Ten-cell bar**: `cells = (pct + 5) / 10`, clamped to 1-10. Zero when invalid or pct is 0 (`:45`). The cells sweep in over `14 + cells` frames with `EASE_STEADY` (`:50-57`, `:74-77`).
- **State line** at y 268: `ON USB POWER` (external > 0), `ON BATTERY` (external == 0), `POWER SOURCE UNKNOWN` (external < 0) (`:81-83`). There is no CHARGING output.
- **Rail line** at y 318: `RAIL x.xx V` from `rail_mv`, or `RAIL --` (`:85-87`).
- **Freshness line** at y 350: `LEVEL SHOWN WHEN UNPLUGGED` (invalid, USB seen), `LEVEL SETTLING` (invalid otherwise), `UPDATED JUST NOW` (age under 2 s), `UPDATED n S AGO` (`:88-91`).
- The state, rail and freshness lines fade in 0.25 s after the hero, over 5 frames (`:79`).
- No input handler and no flags (`:94`: input 0, flags 0). Taps do nothing and the page does not keep the display awake. The engine back swipe still closes it, because `SCR_NOBACK` is not set (`nos/nos.c:852`, `:861`).

## Reads

Service getters:

- `svc_sys()` returns `svc_sys_t` (`nos/svc.h:179-185`).
  - `battery_pct` (`set_battery.c:39`): used only when > 0, otherwise `hal_battery_percent()`.
  - `charging` (`:40`): this is `external_power == 1`, which is "USB host seen", not charge current (`nos/svc.c:992`).
  - `rail_mv` (`:43`): fallback only.
  - The mirror is refreshed once a second by `sys_tick()` (`nos/svc.c:987-995`), from the 1 Hz gate in `svc_tick` (`nos/svc.c:1315-1317`).
- `svc_sys()->battery_valid` is not read here. It is written at `nos/svc.c:994` and never read in NOS.

HAL calls:

- `hal_power_get(&p)` (`core/hal.h:38-41`; implementation `firmware/main/hal_esp32.c:378-387`). The page reads `external_power` (`set_battery.c:41`), `battery_valid` (`:42`), `rail_mv` (`:43`) and `age_ms` (`:44`). It does not read `sample_valid` or `battery_percent` directly.
- `hal_battery_percent()` (`core/hal.h:32`, `hal_esp32.c:392`): fallback only (`set_battery.c:39`).

Engine and drawing (no data): `nos_enter_t` (`nos/nos.h:189`), `nos_t`, `m_frame_k`, `m_clamp`, `m_ink`, `bezier_ease` with `EASE_STEADY`, `dot_text_anim`, `prog_bar(PROG_SQUARES)`, `ui_title`, `ui_state`, `ui_label`, `ui_dim`. `nos_motion_probe` is a no-op unless `GFX_COST_MODEL` is set (`nos/nos.h:330-332`).

Not read: any `nos_prefs` key, any `prefs_*` call, any NVS key, any SD file, any sports journal record, and the BLE battery log ring.

## Writes

The page writes nothing outside its own static `BT` struct (`set_battery.c:27-32`). That struct is RAM only and is refreshed on enter (`:59`). The file calls no `prefs_save`, no `hal_store_*`, no `sports_emit` and no service mutator.

The data it displays is written by other subsystems:

- `hal_esp32_sample_power()` writes the RAM snapshot `s_power` (`firmware/main/hal_esp32.c:365-368`). It emits `SP_RAIL` every second (`:370`) and `SP_BATTERY` when the external or valid state changes (`:372-373`).
- `sports_tick` emits a minute `SP_BATTERY` and `SP_ACTIVITY` (`nos/sports.c:384-390`).
- The `bat_cal` NVS key is written only by the serial command `batcal` (`firmware/main/nos_main.c:1607`, `firmware/main/hal_esp32.c:311-315`).
- `BG_BATTERY` is published to the background store (`nos/background.c:135-137`).
- The companion receives a status line (`nos/svc.c:162-170`).

## Background behaviour

| Work | Runs in | Period | Runs with the page closed | Runs with the screen off | Evidence |
|---|---|---|---|---|---|
| ADC sample and validity state machine (`hal_esp32_sample_power`) | `board-sense` task, priority 2, 4096-byte static stack, `tskNO_AFFINITY` | 1 s (`power_at = now + 1 s`), inside a 20 ms loop | Yes | Yes | `firmware/main/board.c:967`, `:1021`, `:1023-1025`, `:1036` |
| Parked during the stop/sleep handshake | same task | - | - | - | `firmware/main/board.c:972-983` |
| Boot-time first sample | boot path | once | - | - | `firmware/main/board.c:1073` |
| `sys_tick`, `act_tick` and `sports_tick` | `svc_tick` | 1 s gate | Yes (`services_tick`) | 1 Hz (see below) | `nos/svc.c:1315-1319`, `nos/apps/registry.c:62-66` |
| Screen on: services each frame | `app_main` frame loop (`nos/nos.c:2341` inside `frame_run`) | per frame | Yes | - | `firmware/main/nos_main.c:1899`, `nos/nos.c:2299`, `:2341` |
| Screen off: services once per second | `app_main` (`nos_bg_tick(1.0f)`) | 1 s | Yes | Yes | `firmware/main/nos_main.c:2091-2094` |
| AOD / sensor idle: frame run once per second | `app_main` (`nos_frame_run`) | 1 s | Yes | Yes | `firmware/main/nos_main.c:2114-2116` |
| Journal SD writer `sports_sd` | task, priority 1, 4096-byte PSRAM stack, drains at least every 1 s | 1 s | Yes | Yes | `nos/sports.c:282-293`, `:323` |
| Background BG_BATTERY publish | `services_tick` to `background_tick` | 1 s throttle | Yes | Yes | `nos/background.c:160-166`, `:135-137` |
| Low-battery policy | `app_main` loop, each iteration | every loop | Yes | Yes | `firmware/main/nos_main.c:2029-2036`, `nos/power_policy.h:6-11` |
| Companion status | `phone_tick` (`nos/svc.c:1215`) | on version handshake, every 10 min, 5% change, charger change | Yes when phone linked | Yes when phone linked | `nos/svc.c:1231-1233`, `:1247-1252`, `:162-170` |

The page's own `bt_tick` runs `bt_sample()` every frame, but only while the page is on top or in a transition (`set_battery.c:60-63`, `nos/nos.c:2101-2108`). When the page is closed, none of its code runs.

On each state change, `go_state` calls `hal_battery_set_awake(st == PWR_ON)` (`firmware/main/nos_main.c:701`). The estimate then adds a load allowance of +25 mV while awake and +10 mV while not (`firmware/main/hal_esp32.c:361`).

Low-battery policy (`firmware/main/nos_main.c:2029-2036`): entry at 15% or below, and exit at 20% or above, and only on battery with a valid sample (`nos/power_policy.h:6-11`). Below that, the watch enters the display saver. In AOD or sensor-idle it forces display-off (`nos_main.c:2036`). A toast appears only while the screen is on (`:2034`).

## Persistence

- The page itself persists nothing. Its state is `BT` in RAM (`set_battery.c:27-32`).
- The sampler snapshot `s_power`, the stable counters and `s_smooth_mv` are RAM only (`firmware/main/hal_esp32.c:287-299`). A reboot clears them.
- Calibration `bat_cal` is an int clamped to +/-400 mV (`hal_esp32.c:311-315`). It is stored in NVS namespace `wos`, key `bat_cal` (`hal_esp32.c:181`; routing at `:236`). It is loaded lazily at the first sample (`:318`). It is written only by serial `batcal MV` (`firmware/main/nos_main.c:1607`, dispatcher `cmd_exec` at `:1180`). There is no UI.
- On B, the SD journal holds the data the page's history view uses. `SP_BATTERY` goes to `/sdcard/watch/sports/YYYYMMDD-events.spt`. `SP_RAIL` goes to the hourly raw file `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt` (`nos/sports.c:25`, `:215-217`, `:254-261`; `raw_type` at `:56`; enum at `nos/sports.h:8-11`). Nothing in `sports.c` removes or renames these files.
- `essential()` marks `SP_BATTERY` and `SP_RAIL` records with an invalid level or sample as priority records (`nos/sports.c:122-128`, `:137-140`).
- The BLE battery log ring (`hal_esp32.c:1228-1287`) is RAM only and is not filled in the NOS build. Its only caller is `firmware/main/main.c:1172`, and `main.c` is not in the NOS source list (`firmware/main/CMakeLists.txt`, which lists `nos_main.c`). The dump on subscribe returns early on an empty ring (`hal_esp32.c:1249`).

## Navigation

Opened from:

- **Settings > Battery** row (`nos/apps/set_settings.c:483`). Selecting it pushes the page with `nos_push(&scr_battery, TX_PUSH)` (`:1856`). The row value shows the svc percent, or "Charging" when `svc` says USB is seen (`:602-605`).
- **Control-centre header** (time and percent). A tap at y < 84 within 110 px of centre, when no tile is hit and no recording is active, calls `nos_open(&scr_battery, ...)` (`nos/apps/sys_control.c:718-719`). `nos_open` pushes with the iris transition from the tap point (`nos/apps/registry.c:45-52`).

Opens nothing. Exit is the engine back swipe (`nos/nos.c:852`, `:861`). There is no on-screen control.

Not linked from this page:

- BATTERY HISTORY (`scr_battery_history`, `nos/apps/fit_energy.c:284`). It is reached from the control-centre quick tools (`nos/apps/sys_control.c:495`) and from a BG_BATTERY tap on the background/Jobs screen (`nos/apps/sys_background.c:216`).
- Always-on display and screen-timeout rows in Settings (`nos/apps/set_settings.c:455-457`).
- The long-press power sheet `scr_power` (`nos/apps/sys_power.c`), which has its own "CHARGING / BATTERY" title (`sys_power.c:290`).

Simulator: the simulator's app list maps "battery" to the history screen, not to this page (`sim/nos_sim.c:74`).

## Gaps

Each gap below is a finding from source reading. The hardware behaviour has not been measured.

1. **[high] No real charge state.** "ON USB POWER" means a USB host was seen (`firmware/main/hal_esp32.c:323`, `:337`), not that the cell is charging. B has no charge-status pin. `PIN_CHG_STAT` is defined only in the C block (`firmware/main/pins.h:33`), and the B block defines none. Settings says "Charging" for the same condition (`nos/apps/set_settings.c:604`) and the power sheet says "CHARGING" (`nos/apps/sys_power.c:290`). The page cannot show charging, not charging, or full.

2. **[high] Charging without a USB host is not detected.** The only external path without a host is a rail at or above 4.4 V for three samples, and only with ADC calibration (`hal_esp32.c:347-350`). A charger that holds the rail at or below 4.25 V reads as ON BATTERY with a percentage (`:355-361`). Between 4.25 V and 4.4 V the page reports POWER SOURCE UNKNOWN and LEVEL SETTLING (`:355-365`). This is a code reading and has not been measured on hardware.

3. **[high] The level is hidden while plugged in, but shown elsewhere.** The hero reads `--` (`set_battery.c:46-47`). The control-centre header (`nos/apps/sys_control.c:344-346`) and the Settings row (`nos/apps/set_settings.c:602-603`) show `svc_sys()->battery_pct` regardless of validity. A user sees a percentage in the header and `--` on this page for the same moment.

4. **[medium] The percentage is an uncalibrated estimate.** It uses an open-circuit-voltage table plus a fixed load allowance (`hal_esp32.c:300-308`, `:361`). The only correction is serial `batcal MV` (`firmware/main/nos_main.c:1607`). No screen calls `hal_battery_cal_set`; a grep of `nos/apps` finds none.

5. **[medium] Level lost silently if ADC calibration fails.** If `adc_cali_create_scheme_curve_fitting` fails, `s_cali` is NULL (`firmware/main/hal_esp32.c:2344-2345`). That disables the rail-based USB path (`:347`) and the battery path (`:355`). `battery_valid` is then never set, so the hero stays `--` and the page stays at LEVEL SETTLING on battery, with no message.

6. **[medium] Boot placeholder leaks into the header and Settings.** `s_power.battery_percent` starts at 50 (`hal_esp32.c:287`). `svc` copies it unconditionally (`nos/svc.c:991`). It stays until the first percent is assigned, which happens at once on USB with a valid rail (`hal_esp32.c:354`) or after about 10 stable battery seconds (`:355-361`). The Battery page masks this; the header and Settings row do not.

7. **[medium] Stale rail voltage with no age.** After 3 s, `hal_power_get` clears only the validity fields and keeps `rail_mv` and `battery_percent` (`hal_esp32.c:385-387`). The page shows `RAIL x.xx V` from the last good sample (`set_battery.c:85-86`). In the invalid state the freshness line says only LEVEL SETTLING or LEVEL SHOWN WHEN UNPLUGGED (`:88`), so the voltage's age is never shown.

8. **[medium] The companion gets a percentage the watch hides.** `send_status` sends `p.battery_percent` without checking validity (`nos/svc.c:162-170`). It is sent on the version handshake and every 10 min, 5% change or charger change (`:1231-1233`, `:1247-1252`). On USB that percentage is the rail-derived estimate (`hal_esp32.c:354`), which the page's own header comment calls unreliable (`set_battery.c:8-10`).

9. **[medium] Low-battery state is not visible here.** The page does not show that the display saver is active, its thresholds, or any low-battery warning. The saver turns on at 15% or below and off at 20% or above (`nos/power_policy.h:11`). It forces display-off in AOD and sensor-idle (`firmware/main/nos_main.c:2036`). The only feedback is a toast while the screen is on (`:2034`), and there is no setting.

10. **[medium] Expected battery features are missing.** Most are limited by the hardware, which has no fuel gauge (`core/hal.h:34-37`). Missing: time remaining, battery health or cycle count, capacity (the 2 x 420 mAh cell figure is documented only, `nos/hal_ext.h:18`, and no code uses it), charge-complete state, and any per-feature usage breakdown on this page. The mode data is recorded (`nos/sports.c:413-417`) but only BATTERY HISTORY draws it (`nos/apps/fit_energy.c:284`). The page has no history of its own.

11. **[low] No way to reach related screens.** The page has no controls (`set_battery.c:94`, input 0). BATTERY HISTORY and the sensor-idle toggle (`nos/apps/fit_energy.c:247`) are reachable only from other screens. Always-on and screen-timeout settings are in Settings (`set_settings.c:455-457`).

12. **[low] The freshness line almost always reads "UPDATED JUST NOW".** A valid sample is at most 3 s old by construction (`hal_esp32.c:385-387`), and it is taken every 1 s (`firmware/main/board.c:1021`). The `UPDATED n S AGO` branch (`set_battery.c:90`) is reachable only in the 2-3 s band.

13. **[low] `svc_sys().battery_valid` is written and never read.** It is set at `nos/svc.c:994` and declared at `nos/svc.h:180`. The header, Settings and power sheet therefore ignore validity. This is the root of gaps 3 and 6.

14. **[low] The battery log ring is dead in the NOS build.** `hal_batt_log_tick()` is defined at `hal_esp32.c:1270`. Its only caller is `main.c:1172`, which is not compiled into NOS (see Persistence). The ring stays empty, and the BLE dump does nothing.

### Docs and comments that disagree with the code

- `set_battery.c:5` says the state can be CHARGING. The code never outputs that word (`:81-82`).
- `core/hal.h:45-47` says `hal_batt_log_tick()` runs every main-loop iteration. In the NOS build it runs nowhere (gap 14). `docs/os-map/systems/Power and battery.md` says it is never called, which is correct for NOS.
- The `charging` field in `svc_sys_t` (`nos/svc.c:992`) and the Settings "Charging" label both mean "USB host seen", not charge current. The sampler's own comment says so (`firmware/main/hal_esp32.c:323`).
- The simulator's `hal_power_get` reports `sample_valid` and `battery_valid` immediately and never reports the unknown state (`sim/nos_hal_sim.c:190-193`). Simulator screenshots of this page therefore overstate what the device shows during settling.
