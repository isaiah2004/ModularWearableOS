---
title: "Power and battery"
tags: [nos, os-map]
updated: 2026-10-23
---

# Power and battery

## Summary

- **Battery model**: the B board has no fuel gauge and no charger-status GPIO. GPIO4 (ADC1 ch3, divider 3) reads the *system rail*. `hal_esp32_sample_power()` runs once per second inside `task-board-sense`, takes 8 raw ADC reads, uses the median, smooths it with a 1/5 EMA (about 5 s time constant) and converts rail mV to a percentage with a 21-point LiPo open-circuit-voltage table plus a stored `bat_cal` offset and a +25 mV (awake) / +10 mV (asleep) load allowance. `firmware/main/hal_esp32.c:284-376`.
- **Valid percentage** only after 10 consecutive 1 Hz stable samples while not on USB. "Charging" is really "USB present": `usb_serial_jtag_is_connected()` (host SOF) or rail >= 4400 mV for 3 samples. Full or charging current cannot be known. `hal_esp32.c:323,347-363`, `docs/PMIC_AUDIT.md`.
- **Power states** (`PWR_ON`, `PWR_AOD`, `PWR_SENSOR`, `PWR_OFF`) live in `firmware/main/nos_main.c` `go_state()`. With the shipped defaults (`sleep_s=15`, `aod=0`, `sensor_idle=1`) the screen never turns off on timeout. It drops to `PWR_SENSOR`, a 1 Hz always-on face at 10 % brightness and an 80 MHz cap. `PWR_OFF` is reached only by the power-sheet SLEEP button, `off` over serial, or the low-battery policy. `nos/prefs.c:149-169`, `nos_main.c:2077-2086`, `nos/power_policy.h:13`.
- **Sleep modes actually used**: ESP-IDF DFS 80-240 MHz (`CONFIG_PM_ENABLE=y`, `light_sleep_enable=false` everywhere), panel DISPOFF+SLPIN (0x28/0x10) when `PWR_OFF`, and **deep sleep only for the explicit "Power off"** (ext0 wake on GPIO0 only; wake is a full reboot). There is no light sleep, no tickless idle (`FREERTOS_HZ=1000`), no BT modem sleep. The IMU INT (GPIO8) and RTC INT (GPIO15) pins are defined but never used. `nos_power.c:30-62`, `nos_main.c:803-811`, `sdkconfig:1005,1576`.
- **Wake sources from OFF/AOD/SENSOR**: touch (polled at 25 Hz), BOOT button (GPIO0 ISR), raise-to-wake (`wake_service` / `motion_wake`, evaluated at 50 Hz), double knock (`knock.c`, 150 Hz detector), and any top screen flagged `SCR_KEEPAWAKE` (alarm/timer/call/find ring screens). All except touch and button are suppressed in low-battery mode. Notifications do **not** light the display.
- **"Background jobs"** (`nos/background.c`, Jobs app) is **not a scheduler**. It is a passive, RAM-only observability monitor of 13 existing services. There is no job queue, budget, priority or start/stop control. Each service keeps its own owner and cadence (table below).
- **Battery history** (`scr_battery_history`) reads 24 time bins from the daily SD sports journal (`/sdcard/watch/sports/YYYYMMDD-events.spt`, `SP_BATTERY` records written every 60 s and on state changes). A BLE-dumped 24 h ring (`hal_batt_log_tick`) exists but is never called.

## Components

### hw-battery-adc / svc-battery (sampler, % model, validity)
- Purpose: produce the single cached `hal_power` snapshot `{rail_mv, battery_percent, battery_valid, external_power, sample_valid, age_ms}`.
- Runs in: `task-board-sense` (4096 B stack, priority 2, no affinity). Called when `now >= power_at`, `power_at = now + 1 s`. `firmware/main/board.c:1021`. Also once in `board_init()` (`board.c:1073`) before the workers exist. Runs with the screen off.
- ADC: `ADC_UNIT_1`, 12 dB attenuation, 12-bit, curve-fitting calibration; B uses `BAT_DIVIDER=3` (`pins.h:65-66`). Samples outside 2500-5600 mV are dropped; at least 5 of 8 must be valid. `hal_esp32.c:327-336`, init `hal_esp32.c:2332-2344`.
- Filter: sort, median, then `s_smooth_mv = (4*s_smooth_mv + measured)/5`. `hal_esp32.c:340-346`.
- External detection: `usb || s_high_stable>=3` where `s_high_stable` counts consecutive medians >= 4400 mV and needs the calibration handle. USB percent uses `lipo_percent(smooth+cal)` when smooth is 3300-4300 mV but `battery_valid` stays 0. `hal_esp32.c:347-355`.
- Battery-valid rule: calibration present, median 3300-4250, smooth 3300-4250, spread of the 8 samples <= 120 mV, for 10 consecutive seconds. Then `external_power=0`, `battery_valid=1`, `percent=lipo_percent(smooth + bat_cal + (awake?25:10))`. A gap > 3 s resets the counters. `hal_esp32.c:321,356-364`.
- Table: `lipo_percent()` is a 21-point OCV curve (3300 mV = 0 %, 3610 = 5 %, 3850 = 55 %, 4200 = 100 %), 5 % per segment, integer interpolation. No hysteresis or monotone-discharge filter. `hal_esp32.c:300-308`.
- Staleness: `hal_power_get()` recomputes `age_ms` and zeroes `sample_valid`, `battery_valid`, `external_power=-1` once older than 3000 ms. `hal_esp32.c:378-388`.
- Writes: `s_power` (critical-section copy), `SP_RAIL` sports record every sample (1 Hz), `SP_BATTERY` on a change of `external_power` or `battery_valid`. `hal_esp32.c:366-374`.
- Calibration: serial `batcal MV` (clamped +-400 mV) saved to NVS key `bat_cal` (namespace `wos`); `bat` prints it. `hal_esp32.c:309-315`, `nos_main.c:1607-1612`. No UI to set it.
- `hal_battery_set_awake(st==PWR_ON)` is called from `go_state` so the load allowance follows the display. `nos_main.c:701`.

### svc-battery-core-adapter (core/battery.c, dead code)
- `battery_update()/battery_mv()/battery_pct()/battery_state()` wrap `hal_power_get`. Compiled into the core lib but **nothing in NOS calls `battery_update()`**; its only callers are legacy `core/wos.c:955` and `tests/battery_test.c`. `core/battery.c:11-29`.

### svc-sys-power-mirror (svc_sys snapshot)
- `sys_tick()` in `nos/svc.c:987-996` copies `hal_power_get` into `svc_sys_t {battery_pct, charging, rail_mv, battery_valid}` once per second (`svc_tick` -> `now - s_slow_ms >= 1000`, `svc.c:1315-1320`). `charging` is `external_power==1`. Consumers use `battery_pct` without checking `battery_valid` (see gaps).
- Companion link: `send_status()` writes `{"t":"status","bat":<pct>,"volt":<v>,"chg":<0|1>}` every 10 min or on a 5 % / charger change while a phone is connected. `svc.c:160-170,1247-1254`.

### svc-power-policy (nos/power_policy.h)
- Pure inline helpers used by the display state machine. `power_low_update(low, hal_power*)`: external power clears low; invalid or stale data keeps the previous state; otherwise enter at `percent <= 15`, leave at `percent >= 20`. `power_idle_target(low, sensor_idle, aod)`: low -> OFF, else sensor_idle -> SENSOR, else aod -> AOD, else OFF. `power_wake_requested`: touch or button always; raise or knock only when not low. `power_timeout_due`: `!no_sleep && !keep && !down && awake > timeout`. `nos/power_policy.h:6-19`.
- The only low-battery behaviours: display goes to OFF instead of AOD/SENSOR, raise/knock wake and raise eligibility are disabled, and toasts "LOW BATTERY / DISPLAY SAVER" / "BATTERY SAVER OFF". `nos_main.c:2030-2064`. Nothing else (radios, sensors, sports logging, brightness) reacts.
- The `age_ms > 120000` clause is unreachable because `hal_power_get` already invalidates at 3 s.

### svc-display-power (go_state: ON / AOD / SENSOR / OFF)
- Owner: the IDF main task running `app_main()` loop (core 0 affinity per `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0`), file `firmware/main/nos_main.c`.
- `go_state()` (`nos_main.c:691-708`): leaving OFF wakes the panel (`panel_sleep(0)` = SLPOUT 0x11 + DISPON 0x29, one extra ~126 ms frame per the 2026-10-15 power work); entering OFF sets brightness 0 then `panel_sleep(1)` (DISPOFF 0x28 + SLPIN 0x10 through a `JOB_SLEEP` job on the flush queue, `nos_main.c:187-209,686-690`); `cpu_mode(240)` for every state except OFF; touch task poll rate `s_touch_fast` (10 ms when ON, 40 ms otherwise); `hal_battery_set_awake`; `sports_power()` emits an `SP_POWER` record (mode, CPU MHz, brightness); `diag_log_event(DIAG_POWER_STATE, old, new, low)`.
- Loop behaviours (`nos_main.c:2090-2130`):
  - **OFF**: `cpu_mode(80)`, no frames. Once per 1000 ms calls `nos_frame++` and `nos_bg_tick(1.0f)` (services). If the top screen has `SCR_KEEPAWAKE` it returns to ON. Prefs persistence is allowed (`prefs_tick`) and board background persistence is enabled. `vTaskDelay(20 ms)` -> 50 Hz polling of wake conditions.
  - **AOD / SENSOR**: one `nos_frame_run` per 1000 ms (also ticks services), brightness `aod_level()` (default 10 %), `vTaskDelay(20 ms)`. SENSOR additionally caps `max_mhz` to 80.
  - **ON**: frames at the FPS cap, `nos_bg_tick` every frame, persistence deferred until input is idle (`prefs_set_persistence_allowed(1)` windows).
- Timeout: `nos_awake_ms() > sleep_s*1000` (default 15 s, choices 5/10/15/30/60 in Settings) unless `SCR_KEEPAWAKE` top screen, finger down, or serial `awake` override. `nos_main.c:2077-2086`; `nos.c:585-586` (`nos_keep_awake`).
- Brightness: `user_level()` maps 1-100 % to panel 8-255 (`6 + pct*2.49`), multiplied by the ambient factor `s_auto_k` (APDS light sensor, sampled every 500 ms, log response 0.35x-1.4x). `nos_main.c:644-675`.
- Defaults that matter: `aod=0`, `sensor_idle=1`, `raise_wake=1`, `knock_lock=1`, `sleep_s=15`. `nos/prefs.c:140-163`. The `sensor_idle` toggle is only reachable from the Battery History screen button ("IDLE 80MHz ON/OFF", `fit_energy.c:155,247`), not from Settings.

### svc-pm-dfs (nos_power.c, esp_pm)
- `nos_power_init()` (called first in `app_main`, `nos_main.c:1913`): first fixed 240 MHz, then creates two `ESP_PM_CPU_FREQ_MAX` locks (`nos-ui`, `nos-prewarm`), acquires `nos-ui`, then enables DFS `max 240 / min 80 MHz, light_sleep_enable=false`. Any failure falls back to fixed 240 MHz. `firmware/main/nos_power.c:30-62`.
- `nos_power_active_begin/end()` bracket each main-loop iteration (input, wake, housekeeping, render, DMA fence) so the CPU can drop to 80 MHz in the frame-wait / `vTaskDelay` gap. `nos_main.c:1979,2102,2126,2265`.
- `nos_power_bake_begin/end()` is held by `dotbake_task` on core 1 during the dot-mask prebake. `nos_main.c:494-502`.
- `nos_power_set_max_mhz(80|240)` re-configures `esp_pm` (`max=mhz,min=80`); used by `cpu_mode()` for SENSOR and OFF. `nos_power.c:93-102`, `nos_main.c:634-646`.
- Stats / report: `NOSPM ...` serial line (`nos_power_report`). `nos_power.c:103-120`.

### svc-motion-wake + svc-wake-service (raise-to-wake)
- `wake_service_tick(now, eligible)` is called on every main-loop pass (not only when asleep) from `nos_main.c:2038`; `eligible = pwr!=ON && raise_wake && !locked && !low_battery`. In OFF/AOD/SENSOR the loop runs at 50 Hz, in ON at the frame rate. It reads `hal_imu_sample()` (`board.c:486-494`: the 3 Hz-filtered `s_sample`, valid if < 150 ms old), skips duplicates by timestamp, and feeds `motion_wake_update()`.
- Filter feeding it: `imu_take()` smooths raw 500 Hz data with a one-pole (tau 53 ms, ~3 Hz) filter on each of ~150 reads/s. `board.c:296-313`.
- State machine (`core/motion_wake.c`): reset on invalid, free-fall/impact (|a| < 0.35 g or > 2.5 g) or a > 200 ms gap (lines 29-40). Steady = `|a|-1 < 0.07 g`, jitter < 0.025 g, raw gyro < 12 dps (55). Pickup arms after 500 ms steady; wrist-turn arms after the gravity-to-view dot stays < 0.5 for 250 ms (77-81). A candidate starts after 40 ms of motion (speed > 22 dps or linear > 0.12 g) (87). Sufficient motion: turn needs 20 degrees and origin-gravity dot < 0.91; pickup needs 12 degrees or peak linear > 0.12 g for 100 ms (107-110). It must end inside a 55 degree viewing cone (`view_dot > 0.5736`) with linear < 0.15 g, speed < 45 dps, jitter < 0.08 g, held 160 ms (113-116). Cancels: 3 gyro reversals, > 180 degrees, 1800 ms timeout (97), linear > 0.9 g or speed > 600 dps (74, enters a 2 s cooldown). Gyro bias is learned after 1.5 s steady (59-61).
- Config blob `wake_blob {magic 0x57414b31, version 1, flags (bit0 pickup, bit1 turn), wrist, learned mask, view[2][3] (milli-g), FNV-1a checksum}`, NVS key `wake_cfg`, saved with `hal_store_save_checked`. `core/wake_service.c:8-50,60-70`.
- Wrist sync: `wake_wrist_sync()` pushes `nos_prefs.wrist_left` into the blob (retry 1 s). `nos_main.c:939-950`. "Learn reading angle" (Settings, `set_settings.c:1834-1845`) and the Wrist Tuning trial call `wake_service_learn()`: 10 s limit, 800 ms and 12 stable samples (|a| 0.9-1.1 g, speed < 12 dps, delta^2 < 0.0016) to store the reading direction. `wake_service.c:80-87,100-123`.
- Telemetry: `SP_WAKE` record on every stage change or trigger. `nos_main.c:2040-2044`. Serial `wake`, `wake trace S`, `wake learn`.

### svc-knock (double knock to lock / wake)
- Detector `core/knock.c` is fed raw (unfiltered) accel+gyro magnitude by `knock_feed()` inside `imu_take()` from `task-board-sense` (about 150 Hz: one read per 20 ms cycle plus two extra at +8 ms and +16 ms). `firmware/main/board.c:269-283,316-322,1023-1024`.
- Rules: gravity low-pass (150 ms, frozen while |hp| >= 0.30 g), high-passed magnitude > `threshold_g` (default 0.60 g, tuneable 0.20-1.2) is an onset; 70 ms refractory; 250 ms quiet (<0.15 g, <90 dps) before the first knock (tuneable 80-400); second onset 80-520 ms later; 200 ms quiet after, reached within 900 ms. Result is counted in `s_knocks` (`hal_knock_count`). `core/knock.c:13-82`.
- Tuning: `sports_tuning {walk_g, knock_g, walk_peaks, quiet_ms}` from `tuning_get()` (NVS `sportstune`), copied on every sample. `board.c:271-273`, `nos/tuning.c:18-40`.
- Consumers: (1) `nos_main.c:2047-2049,2061` wakes the display from OFF/AOD/SENSOR when `nos_knock_enabled()` (pref `knock_lock`, default on) and not low battery; (2) `nos.c:646-653` `knock_tick()` toggles `nos_locked` on the next engine frame. A knock-wake from an unlocked off watch therefore also **locks** it.

### task-board-sense (power-relevant duties)
- Core: none (`tskNO_AFFINITY`), priority 2, 4096 B static stack in PSRAM. Loop: read IMU, pedometer update, activity snapshot, 1 Hz `hal_esp32_sample_power`, then `vTaskDelay 8 ms; knock_poll; 8 ms; knock_poll; 4 ms` -> 20 ms cycle, about 150 IMU reads/s. Runs always, including screen off, until `board_prepare_sleep()` parks it. `board.c:978-1030,1036`.
- Each read also drives `sports_imu()` (SD journal), `workout_motion_sample()`, `wrist_detect_feed()`, the 3 Hz filter and `knock_feed()`. IMU configured once for 500 Hz accel/gyro with LPF (`board.c:431-438`) and never slowed when the display is off.

### task-board-fs (power-relevant duties)
- Priority 1, 100 ms loop. Saves the pedometer blob (NVS `pedometer`) every 60 s only when `s_background_persistence` is set (i.e. display not ON; retry 5 s on failure), maintains the RTC, and appends one CSV row per 60 s to `/sdcard/watch/logs/<unique>.csv` (header `utc_epoch,uptime_ms,system_mv,imu_valid,ax_g,...,gz_dps`, rotates at 1 MiB). `board.c:921-964,713-748`.

### svc-shutdown (Power off / Restart / deep sleep)
- Entry: power sheet -> `nos_host_power(1|2)` (`nos_main.c:751-762`). Guarded by `shutdown_guard()` (OTA, voice chat, IMU recording/stream/file, mic recording, running or unsaved workout, track recording/saving, SD storage dirty). Then `sports_quiesce(1)`, force ON, `prefs_save()`.
- `shutdown_tick()` each loop pass: wait for assets, flush preferences (`prefs_flush`), `hal_ext_shutdown_begin/poll`, `board_prepare_sleep()` (parks `task-board-sense` and `task-board-fs`, final pedometer/RTC/sports flush, IMU CTRL7=0, SD unmount), then panel sleep, 150 ms, and either `esp_restart()` (mode 1) or `esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); esp_deep_sleep_start()` (mode 2). `nos_main.c:772-811`, `board.c:1110-1167`.
- A button press during phases cancels (`shutdown_cancel`, `nos_main.c:711-729`). Phase 3 = storage restart required, UI mutations stop; serial `power restart` recovers.
- Deep sleep has no wake source other than the BOOT button; wake is a cold boot.

### sys-power (long-press button sheet, `scr_power`)
- Opened by `IN_BTN_LONG` (600 ms, `nos.c:592,953-956`). Shows `svc_sys()->battery_pct` as a dot-matrix hero over ten cells (`cells=(pct+5)/10`, at least 1) and the title `CHARGING` when `svc_sys()->charging` (USB present) else `BATTERY`. Actions: SLEEP (home, then `nos_host_power(0)` = `go_state(PWR_OFF)`), RESTART and POWER OFF (arm for 3 s, second tap commits, `nos_host_power(1|2)` after 0.8 s). `nos/apps/sys_power.c:164-188,290,350-365`.
- Does not check `battery_valid`; shows the placeholder or USB-biased number as fact.

### sys-battery (Settings > Battery, `scr_battery`)
- Pull model: `bt_sample()` each tick reads `hal_power_get` + `svc_sys`; hides the number (`--`) and cells when `!battery_valid`; shows `ON USB POWER / ON BATTERY / POWER SOURCE UNKNOWN`, rail volts and sample age. Honest wording ("LEVEL SHOWN WHEN UNPLUGGED"/"LEVEL SETTLING"). `nos/apps/set_battery.c:34-92`. Opened from Settings row (`set_settings.c:1856`) and the control-centre header.

### app-battery-history (`scr_battery_history`)
- Same code as Energy (`kind=1`) in `nos/apps/fit_energy.c`. 24 bars over a day or a zoomed window (drag across bars to zoom). Metric toggles LEVEL % (max fixed at 100) vs VOLTAGE mV. A coloured stripe under each bar shows the power modes seen (white = ACTIVE, grey = other, red = external power seen). `fit_energy.c:94-125,135-156`.
- Data path: `sports_query_request(day,start,end)` (asynchronous; worker `sports_sd`) scans `/sdcard/watch/sports/YYYYMMDD-events.spt` and `bin_record()` averages `SP_BATTERY` records into 24 bins (level only when `battery_valid`, mV when `sample_valid`, external flag, mode mask). Result polled each tick, refreshed every 10 s. `sports.c:408-420,500-560` (query worker `query_step`), `fit_energy.c:62-80`.
- The same screen hosts the `sensor_idle` toggle and a MODE LOG button into Sensor logs.

### svc-sports-logger (battery records)
- `sports_tick()` (1 Hz internally, via `svc_tick`) emits `SP_BATTERY {pct, mV, external, batt_valid, sample_valid, mode, age}` every 60 s and `SP_ACTIVITY`. Board task emits `SP_BATTERY` on state change and `SP_RAIL` each second, `SP_POWER` on display state change. `nos/sports.c:384-392,193-195`, `hal_esp32.c:366-374`. `SP_POWER/BATTERY/WAKE/LOCK` are "essential" and bypass shutdown quiesce. Writer `sports_sd` drains once per second to the events file (non-raw types) or hourly `-HH-sensors.spt` (raw types incl. `SP_RAIL`, `SP_IMU`). `sports.c:56,106-128,206-247`.

### svc-background-jobs (nos/background.c)
- Passive monitor, `background_init()` allocates `13 x job_data` (about 2.8 KB each, PSRAM via `nos_mem_alloc("background.history")`) and registers `sports_observe(observe)`. `background_tick()` is called from `services_tick` (registry.c:62-66), which is `nos_bg_tick`: every frame in ON, 1 Hz in OFF/AOD/SENSOR. Internally gated to once per second. `nos/background.c:117-125,160-185`, `nos/apps/registry.c:62-73`.
- `sample_services()` polls (1 Hz): steps/calories (`svc_activity`, `hal_activity_get`), battery (`hal_power_get`, timestamped `now - age_ms`), sports journal status, diag-log status, IMU recorder, workout snapshot. Motion, climate, light, compass, heart and GPS arrive by **push** from the sports observer at acquisition time (`observe()` on `sports_emit`, including when the SD queue is full). `background.c:90-158`.
- State per job: WAITING, RUNNING, IDLE, NO MODULE, STALE, ERROR, PAUSED, SAVING; `stale_ms` per job; one point per second into a 120-point ring, 16-event change log; both RAM only, cleared on reboot. `background.c:21-35,56-75`.
- Thread safety: `portMUX` critical sections; observer called from `task-board-sense` at about 150 Hz for BG_MOTION.

| id | Job | Source / producer | Cadence | stale_ms | on-demand |
|---|---|---|---|---|---|
| 0 | Motion sensor | `SP_IMU` from `imu_take` (board-sense) | ~150 Hz push, 1 point/s | 2000 | no |
| 1 | Step tracking | `hal_activity_get` + `svc_activity` | 1 Hz check | 3000 | no |
| 2 | Calorie estimate | `svc_activity()->kcal` (0.04 kcal/step) | 1 Hz check | 3000 | no |
| 3 | Battery sampling | `hal_power_get` | 1 Hz | 4000 | no |
| 4 | SD sensor journal | `sports_get_status` | 1 Hz (heartbeat `last_service_ms`) | 10000 | no |
| 5 | System trace | `diag_log_get` | 1 Hz | 0 (never stale) | no |
| 6 | IMU recording | `hal_imu_record_get` | 1 Hz | 3000 | yes |
| 7 | Sports session | `workout_get` | 1 Hz | 3000 | yes |
| 8 | Heart sampling | `SP_HEART` push (EXT_HR) | ~60 s | 4000 | yes |
| 9 | GPS receiver | `SP_GPS` push (EXT_GPS) | on demand | 5000 | yes |
| 10 | Climate sensor | `SP_ENV` push (EXT_ENV) | acquisition | 7000 | no |
| 11 | Light sensor | `SP_LIGHT` push (EXT_LIGHT) | acquisition | 3000 | no |
| 12 | Compass sensor | `SP_MAG` push (EXT_MAG) | on demand | 3000 | yes |

### app-jobs (sys_background.c, launcher "Jobs")
- Overview: 4 rows per page, 4 pages (13 jobs), state colour dot, last value, tap to detail. Detail: GRAPH (2 min), READINGS, EVENTS tabs; tap value to cycle metric; tap graph to freeze and inspect; OPEN RECORDS jumps to IMU, Activity, Energy, Battery History, Developer, Workout or Sensor logs. `refresh()` copies snapshots once per second in `tick`, `draw` never polls. `nos/apps/sys_background.c:62-80,193-258`. Serial `jobs` prints the same data without waking the display. `nos_main.c:1193-1204`.

### svc-battery-ble-log (hal_batt_log_tick, dead code)
- Designed as a 1440-sample (24 h at 1/min) mV ring dumped delta-compressed over the BLE media characteristic (`0xB1` header, `0xB2` chunks, `0xB3` end) on subscribe or every 2 min. `hal_esp32.c:1222-1287`, subscribe flag `hal_esp32.c:1454`. **`hal_batt_log_tick()` has no caller in the NOS build** (only `core/hal.h:48` comment and sim stubs), so the ring is never filled or dumped. `tools/watch_media_bridge.py` still expects it.

## How it runs

**Tasks and cores (power relevant)**

| Task | Core / prio | Period | Screen off |
|---|---|---|---|
| IDF main (`app_main` UI loop) | core 0, inherited prio | ON: frame rate; AOD/SENSOR: 20 ms loop with 1 Hz frame; OFF: 20 ms loop with 1 Hz services | yes (all wake logic, `wake_service_tick`, `diag_log_tick`, 1 Hz `svc_tick`+`background_tick`) |
| `board-sense` | any core, prio 2 | 20 ms cycle, 3 IMU reads, 1 Hz power sample | yes, always |
| `board-fs` | any, prio 1 | 100 ms; 60 s pedometer save and CSV row | yes (pedometer save only while display not ON) |
| `touch` | core 1, prio 5 | 10 ms ON, 40 ms otherwise | yes (25 Hz poll is the touch wake path) |
| `sports_sd` | any, prio 1 (PSRAM stack) | drain every 1 s, queries every 20 ms when pending | yes |
| `ext` (straps) | any, prio 0 | up to 50 ms | yes |
| `flush` | core 1, prio 4 | idle when no frame; handles `JOB_SLEEP` | panel sleep job |
| `dotbake` | core 1, prio 1 | boot only, holds `nos-prewarm` lock | no |
| ESP timer (frame pace) | esp_timer | per frame | stops in OFF/AOD |
| GPIO0 ISR | ISR | edge, 25 ms debounce | yes (button wake) |

**CPU frequency**: DFS range 80-240 MHz. `nos-ui` lock keeps 240 MHz only inside each loop iteration. OFF caps the maximum to 80 MHz, SENSOR caps to 80 MHz after the first frame, AOD and ON allow 240.

**Screen-off schedule (PWR_OFF)**: panel in SLPIN, CPU <= 80 MHz, UI loop wakes every 20 ms, board-sense every 20 ms (IMU at ~150 reads/s, knock and step detection, wrist detect, raw journal to SD), touch every 40 ms, ext every <= 50 ms, 1 Hz services (Gadgetbridge line parsing, alarms, timers, weather fetch scheduling, activity, sports minute records, background monitor, heart tick every 60 s), BLE stays up, Wi-Fi only if the saved pref is on. No light sleep.

**Interaction with other numbers (not measured)**: `docs/SPORTS_UPDATE.md` records that in a 139-minute unplugged window SENSOR_IDLE was 98 min and ACTIVE 41 min and no OFF state was observed. `build/power/POWER_REVIEW.md` (memory note) estimated about 56 mA screen-off before the 2026-10-15 quick wins; nothing here has been re-measured with a current meter.

## Data in / data out

| Data in | Source | Consumer |
|---|---|---|
| Rail ADC (8 samples/s burst) | hw-battery-adc GPIO4 | svc-battery |
| USB SOF state | `usb_serial_jtag_is_connected()` | svc-battery |
| `bat_cal` mV offset | NVS `wos/bat_cal` | svc-battery |
| IMU 3 Hz-filtered sample | task-board-sense | svc-wake-service |
| IMU raw 500 Hz reads | hw-qmi8658-imu via board-sense | svc-knock |
| `knock_g`, `quiet_ms` | `nos/tuning.c` (NVS `sportstune`) | svc-knock |
| Touch down flag, button edge | task-touch, GPIO0 ISR | svc-display-power |
| prefs `sleep_s, aod, sensor_idle, raise_wake, knock_lock, aod_bright, brightness, auto_bright, wrist_left` | store-sd-settings / NVS | svc-display-power, svc-wake-service |
| Top screen flag `SCR_KEEPAWAKE` | engine | svc-display-power |
| Light-sensor lux | EXT_LIGHT | svc-display-power auto brightness |
| Sports/diag status, activity, workout, ext samples | services | svc-background-jobs |

| Data out | Destination | Via |
|---|---|---|
| `hal_power` snapshot | svc_sys mirror, set_battery, set_about/diag/modules, background, sports, diag log | `hal_power_get` |
| `svc_sys_t` battery fields | sys_power, sys_control, sys_home/faces, set_settings, Gadgetbridge status | `svc_sys()` |
| `SP_RAIL` 1 Hz, `SP_BATTERY` 60 s + change, `SP_POWER`, `SP_WAKE`, `SP_KNOCK`, `SP_LOCK` | sports journal (SD) | `sports_emit` |
| `DIAG_POWER_SAMPLE` every 30 s, `DIAG_POWER_STATE/POLICY/WAKE/SLEEP_BLOCK` | diag log `/sdcard/watch/diagnostics/diag-NN.bin` | `diag_log_tick` / `diag_log_event` (`nos/diag_log.c:350-360`) |
| CSV row (system_mv + IMU) 60 s | `/sdcard/watch/logs/*.csv` | board-fs |
| Battery % / volts / charging | phone | `send_status` (JSON line) |
| Panel sleep, brightness | CO5300 | flush jobs |

Serial/host: `bat`, `batcal MV`, `wake`, `wake trace S`, `wake learn`, `knock`, `off`, `awake`, `jobs`, `power restart|cancel`, `NOSPM`, `NOSLOW` lines.

## Persistence

| What | Where | Policy |
|---|---|---|
| Battery calibration | NVS `wos` key `bat_cal` (int) | written only by serial `batcal` |
| Wake config / learned view | NVS `wos` key `wake_cfg` (checked blob, FNV-1a) | `hal_store_save_checked` after verification |
| Tuning (knock/walk) | NVS `sportstune` | checked save |
| Preferences (sleep timeout, AOD, sensor idle, raise, knock) | SD `/watch/settings.{0,1}.bin` (authoritative) mirrored to NVS `nosprefs` | debounced 2 s / max 10 s, not during ON motion |
| Battery history | SD `/sdcard/watch/sports/YYYYMMDD-events.spt` (64-byte checksummed records) | writer fsync per second batch |
| Raw rail/IMU samples | SD `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt` | same writer |
| 60 s system-mV CSV | SD `/sdcard/watch/logs/*.csv` | 1 MiB rotation |
| Power diag samples | SD `/sdcard/watch/diagnostics/diag-NN.bin` + RTC retained faults | diag writer |
| Background-job history | RAM/PSRAM only (120 points x 13 jobs + 16 events) | lost on reboot/deep sleep |
| 24 h BLE mV ring | RAM (unused) | n/a |
| Low-battery flag, power state | RAM only | recomputed |
| Last battery % | not persisted | boot starts at placeholder 50 |

## Interactions

- [[Engine and scheduling]]: `nos_bg_tick` hook, `nos_keep_awake`, `SCR_KEEPAWAKE`, frame pacing and `nos_power_active_begin/end` brackets around each loop iteration.
- [[Rendering pipeline]]: panel SLPIN/DISPOFF jobs on the flush queue, AOD brightness and 1 Hz frame.
- [[Services hub]]: `svc_tick` 1 Hz `sys_tick`, `send_status`, `heart_tick`, alarm/timer rings that wake the display.
- [[Health and activity]]: pedometer (`task-board-sense`), `svc_activity` kcal, wrist detect fed from `imu_take`.
- [[Sports and workouts]]: `SP_*` journal, `sports_sd` writer, `workout_get` job, shutdown guard.
- [[Storage and persistence]]: SD settings, NVS keys, sports files, CSV log, diag log, SD unmount in `board_prepare_sleep`.
- [[Hardware and drivers]]: ADC GPIO4, FT3168 polling, QMI8658 500 Hz, GPIO0 button, unused GPIO8/GPIO15 interrupts.
- [[Connectivity and voice]]: BLE stays active while the screen is off; battery status over Gadgetbridge JSON; unused BLE battery-ring dump.
- [[Shell and faces]]: faces and header show `c->battery`/`c->charging` without validity.
- [[Utility apps]]: Jobs, Battery History, Sensor logs launcher entries.
- [[Settings and developer]]: Settings > Battery, timeout/AOD/raise/knock rows, Wrist tuning, Developer tools trace.
- [[Calorie trace]]: BG_CALORIES job mirrors `svc_activity()->kcal`.
- [[Gaps]], [[OS overview]].

## Gaps

1. **High - SD raw IMU journal runs continuously, including screen off.** `imu_take()` calls `sports_imu()` for every read (~150/s); `SP_IMU` is a raw type and goes to hourly `-HH-sensors.spt` at 64 B/record, about 9.6 KB/s (roughly 35 MB/h) with an open+fsync+close batch each second. `docs/BACKGROUND_JOBS.md` itself records 757 motion updates and 656 committed journal records in 5 s with the display off. There is no gate by power state, battery level or user opt-in. Evidence: `board.c:300,1023-1024`, `nos/sports.c:56,206-247`. Impact: SD wear and standby current; contradicts the power-saving intent of SENSOR/OFF.
2. **High - screen never turns off by default.** `sensor_idle=1` takes precedence over `aod` in `power_idle_target`, so after 15 s the panel stays lit in SENSOR mode (1 Hz face at 10 %) even with "Always-on display" off; OFF only appears below 15 % or via SLEEP. Evidence: `prefs.c:149,163`, `power_policy.h:13`, `nos_main.c:2085`; `docs/SPORTS_UPDATE.md` found no OFF state in a 139-minute run. The only toggle for `sensor_idle` is buried in Battery History (`fit_energy.c:155,247`). Impact: OLED always lit; AOD setting appears broken.
3. **High - no light sleep, no tickless idle, no wake interrupts.** `light_sleep_enable=false` in every `esp_pm_config_t`, `FREERTOS_HZ=1000` without tickless idle, BT modem sleep off, IMU INT (GPIO8) and RTC INT (GPIO15) never configured; standby is a 20 ms busy polling loop on three tasks. Evidence: `nos_power.c:20,34,49,97`, `sdkconfig:1005`, `pins.h:55-56`, `docs/PMIC_AUDIT.md` finding 3. Impact: screen-off draw stays tens of mA; `POWER_REVIEW` plan to reach ~45 h is not started.
4. **High - charging and percent are inferred, and UI labels overstate it.** `charging` is "USB present" (host SOF or rail >= 4.4 V), but the power sheet title says CHARGING, Settings shows "Charging" and the phone gets `chg`. A wall charger without data lines at < 4.4 V reads as unknown. No full-charge or charge-current detection exists (STAT is not routed on B). Evidence: `hal_esp32.c:323,347-355`, `sys_power.c:290`, `set_settings.c:602-604`, `svc.c:164,992`. Impact: user told "charging" for a trickle/full cell; percentages on USB are voltage-biased.
5. **Medium - battery percentage shown without validity check.** `sys_power`, Control Center, home faces, Settings row, Modules and the Gadgetbridge status use `battery_percent` directly. On battery, for the first 10 s after boot (and after unplug) the value is the placeholder 50 (`s_power` initial) or the USB-biased last value. Evidence: `hal_esp32.c:287,356-364`, `sys_power.c:168`, `sys_control.c:344,532`, `sys_home.c:474`, `set_settings.c:602`, `set_modules.c:795,1186`, `svc.c:164`. `docs/PMIC_AUDIT.md` finding 2 is still open (Settings > Battery itself was fixed). Impact: false 50 % at boot, wrong low-battery faces.
6. **Medium - no critical-battery action.** Only a display-saver at <= 15 %. No notification, no workout save, no stop of SD logging or radios, no shutdown at the cell cutoff (0 % = 3.3 V). Evidence: `power_policy.h:6-11`, `nos_main.c:2030-2037`. Impact: abrupt brown-out can corrupt SD files or lose unsaved workout data.
7. **Medium - `hal_batt_log_tick()` is never called.** The BLE battery ring (`BATT_N=1440`) never fills, so the companion subscribe dump and `tools/watch_media_bridge.py` get nothing; `core/hal.h:45` comment is stale. Evidence: `hal_esp32.c:1270-1287`; no callers in `firmware/main/nos_main.c` or `nos/`. Impact: dead feature plus ~3 KB RAM.
8. **Medium - `core/battery.c` adapter is dead.** `battery_update()` is called only by legacy `core/wos.c:955`. Evidence: `core/battery.c:11`. Impact: misleading API; dead code in the core lib.
9. **Medium - "Background jobs" is observability only.** No scheduler: jobs cannot be started, stopped, throttled or budgeted, there is no priority or deadline, and history is 2 minutes of RAM that resets on boot. The app name and `docs/BACKGROUND_JOBS.md` can be read as a scheduler. Evidence: `nos/background.c:1-3`, `background.h:45-50`. Impact: no way to cap background cost.
10. **Medium - alarms and timers cannot fire after Power off.** Deep sleep wakes only on GPIO0; the PCF85063 INT (GPIO15) is not configured as a wake source, and alarms are evaluated only in `alarm_tick` inside `svc_tick`. Evidence: `nos_main.c:810`, `pins.h:56`, `svc.c:1326`. Impact: a user who powers off misses their alarm silently.
11. **Medium - raw IMU consumers at 150 Hz hold critical sections and a PSRAM lock path.** `sports_emit`, `background observe()` (two `portENTER_CRITICAL`), `tuning_get`, `workout_motion_sample`, `wrist_detect_feed` all run per sample in `board-sense`. Evidence: `board.c:296-313`, `background.c:76-116`, `sports.c:129-146`. Impact: jitter risk for the sensor loop and CPU cost at 80 MHz in OFF.
12. **Low-Medium - notifications do not wake the display.** `nos_banner()` fires haptics and `nos_keep_awake()` but the OFF loop only wakes for touch/button/raise/knock/`SCR_KEEPAWAKE` screens. In AOD/SENSOR the banner animates only at the 1 Hz frame. Evidence: `nos/nos.c:509-521`, `nos_main.c:2061,2093-2096`. Impact: missed visual cue; stale banner on next wake.
13. **Low-Medium - USB plug-in does not wake or signal.** No display wake, haptic or animation on cable connect. Evidence: `power_wake_requested` inputs, `nos_main.c:2061`. Impact: user cannot see that charging started.
14. **Low - knock wake from an unlocked watch also locks it.** `knock_tick()` toggles `nos_locked` on the first frame after the knock-triggered wake. Evidence: `nos.c:646-653`, `nos_main.c:2047-2049`. Impact: surprising lock when only wanting to wake.
15. **Low - `wake_service_set("pickup"|"turn")` has no UI caller.** Both gestures stay enabled with no way to disable one. Evidence: `core/wake_service.c:64-75`; callers only set `"wrist"` (`nos_main.c:948`, `set_settings.c:1840`). Impact: dead options; `g_setting_tilt_wake` legacy global still written (`wake_service.c:48`).
16. **Low - `SP_DEEP_SLEEP` mode is never emitted.** Deep sleep ends the program; the journal records `SP_POWER` for ON/AOD/SENSOR/OFF only, so battery history cannot show powered-off intervals. Evidence: `sports.h:12`, `nos_main.c:643,666,706`. Impact: gaps in history look like missing data.
17. **Low - `cpu_mode()` retry loop in PM fallback.** If `nos_power_init` fell back to fixed 240 MHz (`s_enabled=0`), `nos_power_set_max_mhz(80)` returns an error every second and emits an essential `SP_ROUTINE` record each retry forever in OFF/SENSOR. Evidence: `nos_main.c:634-646`, `nos_power.c:93-97`. Impact: SD record spam in a degraded mode.
18. **Low - `power_low_update` stale-age clause is dead.** `p->age_ms > 120000` can never be true because `hal_power_get` invalidates at 3000 ms; a stuck sampler therefore holds the previous low flag indefinitely. Evidence: `power_policy.h:9`, `hal_esp32.c:385`. Impact: low state can stick if the sampler stalls.
19. **Low - no percent hysteresis or smoothing across states.** Piecewise-linear table with integer steps; load allowance flips by 15 mV when the display changes state, so the number can step 1-2 % on wake/sleep. Not persisted across reboots, no capacity-based correction. Evidence: `hal_esp32.c:300-308,361`. Impact: wobbly percentage.
20. **Low - stale docs.** `docs/PMIC_AUDIT.md` cites old line numbers and finding 1 is already fixed; `core/hal.h:45` documents a main-loop call that no longer exists; `docs/BACKGROUND_JOBS.md` claims no battery percent on USB (code publishes the value with validity bit 1 clear, so the UI hides it, consistent) but implies a scheduler by name.
21. **Low - deep-sleep wake pin is a strap pin with no hold.** `esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0)` relies on the board pull-up; no RTC-hold, so peripherals powered by the main rail are unknown during sleep and no residual current has been measured (`docs/PMIC_AUDIT.md` finding 4). Evidence: `nos_main.c:810`. Impact: unknown "off" battery drain.
22. **Low - sim does not model power policy.** The simulator only toasts for `nos_host_power` and has no timeout, low-battery or wake state machine, so these paths are covered only by header unit tests (`tests/power_policy_test.c`, `nos_power_test.c`, `wake_service_test.c`, `motion_wake_test.c`, `knock_test.c`, `background_test.c`) and device runs. Evidence: `sim/nos_sim.c:95-102`, `sim/nos_hal_sim.c:190-194`.
