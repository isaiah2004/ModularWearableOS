---
title: "Screen-off and low-power run matrix: what runs per display state"
tags: [nos, os-map]
updated: 2026-10-23
---

# Screen-off and low-power run matrix: what runs per display state

Target: Waveshare ESP32-S3-Touch-AMOLED-1.43-B, NOS build (all of `nos/**/*.c` plus `firmware/main/*`). Legacy `firmware/main/main.c`, `core/wos.c`, `core/apps/*`, `core/pip*.c` are dead and not covered. Everything below is read from code; comments and docs are called out where they differ. Companion note: [[Power and battery]].

## Summary

- There are four display states in `firmware/main/nos_main.c:626` (`PWR_ON, PWR_AOD, PWR_OFF, PWR_SENSOR`). Deep sleep is not a display state, it is a shutdown path (`nos_host_power(2)`).
- With factory defaults (`sleep_s=15`, `aod=0`, `sensor_idle=1`; `nos/prefs.c:140-163`) the idle timeout goes to PWR_SENSOR, never PWR_OFF, because `power_idle_target()` prefers SENSOR over AOD over OFF (`nos/power_policy.h`). PWR_OFF is reached by the SLEEP button, by low battery (<=15 %), or by turning `sensor_idle` off (only toggle: Energy / Battery History, `nos/apps/fit_energy.c:155,247`).
- The UI task (`app_main`, core 0, priority 1) never blocks on an interrupt in any state. ON: paced by an esp_timer to 60 Hz present. SENSOR / AOD / OFF: a 20 ms `vTaskDelay` loop (about 50 Hz wakeups) in which a full frame is rendered only once per second (SENSOR/AOD) or never (OFF). OFF still runs `nos_bg_tick` at 1 Hz.
- Services (`svc_tick` plus `background_tick`) keep running in every state, at 1 Hz in OFF and at frame rate / 1 Hz in AOD/SENSOR. Alarms, workout recording, track recording, phone link parsing, step and calorie accounting all depend on this. Notifications and recording never wake the display.
- The touch task is polled in every state (10 ms ON, 40 ms otherwise); FT3168 has no IRQ on this board, so polling is the only touch-wake path.
- No light sleep, no tickless idle, no wake interrupts. DFS (80 to 240 MHz) is the only CPU power control. The IMU is read about 150 times per second by `board-sense` regardless of display state.
- Deep sleep is entered only by POWER OFF; the only wake source is ext0 on GPIO0 (BOOT, active low). GPIO15 (RTC INT) and GPIO8 (IMU INT) are defined but never used. An alarm cannot fire while powered off and is not reported afterwards.

## Components

### sys-power (screen-off / power sheet) `nos/apps/sys_power.c`
- Purpose: SLEEP / RESTART / POWER OFF buttons.
- Runs: UI task, only while the sheet is on screen. No task or timer.
- Writes: calls `nos_host_power(0|1|2)` (`nos/apps/sys_power.c` about 182-186 and 358-362).
- Behaviour: SLEEP is `go_state(PWR_OFF)` (not deep sleep); RESTART is `esp_restart()`; POWER OFF runs the deep-sleep path below.

### task-ui (app_main loop) `firmware/main/nos_main.c:1977-2290`
- Purpose: input, state machine, frame render, services tick.
- Runs: `app_main` task, core 0 (`CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0`), priority 1, 8192 B stack.
- Cadence per state:
  - ON: esp_timer "pace" (`s_frame_timer`, `pace_cb`) releases the loop each frame; `NOS_PRESENT_FPS=60`, animation `NOS_FPS=40`, `fps_cap` default 60 (`nos/render_config.h`, `nos/frame_cap.h`). Frame render at `nos_main.c:2132-2290`.
  - SENSOR / AOD: `vTaskDelay(20)` per pass; a full `nos_frame_run` plus `frame_drain` only when 1 s has elapsed (`last_aod`, `nos_main.c:2106-2130`). `prefs_set_persistence_allowed(!voice_chat_active())` and `board_set_background_persistence(1)`; SENSOR calls `cpu_mode(80)`.
  - OFF: `cpu_mode(80)`; `vTaskDelay(20)`; no `nos_frame_run`; every 1000 ms `nos_frame++; nos_bg_tick(1.0f)` (guarded by `mutate`) and `prefs_tick` (`nos_main.c:2091-2103`). A top screen with SCR_KEEPAWAKE forces return to ON.
- Every pass (all states): `wake_service_tick`, `radio_preferences` (`nos_main.c:953-961`), `touch_drain`, wake/sleep policy (`nos_main.c:2038-2086`), `hal_setup_poll` (via `firmware/main/watch_setup.inc:33`).
- Reads: touch ring, button, `nos_prefs`, battery percent, wake service output.
- Writes: `s_pwr`, panel brightness/sleep, `nos_aod`, `s_touch_fast`.

### task-touch `firmware/main/nos_main.c:516-590`
- Purpose: poll FT3168 over I2C into a 16-entry ring.
- Runs: core 1, priority 5, 4096 B stack. `s_touch_fast` (set in `go_state`, `nos_main.c:691-707`) selects 10 ms (ON) or `TOUCH_POLL_MS` 40 ms (AOD/SENSOR/OFF).
- Not display-gated: it keeps polling in OFF. `touch_drain(0)` discards samples when not ON; a waking touch is swallowed (`s_swallow`). No reset or interrupt GPIO for FT3168 on B (CLAUDE.md pin table).

### task-flush `firmware/main/nos_main.c` (`boot_resources_init` 1870-1897)
- Purpose: push painted stripes to the QSPI panel.
- Runs: core 1, priority 4, 3072 B stack; blocks on a queue with a 1000 ms timeout. Idle when nothing is painted, so OFF costs nothing here.

### task-cmd `firmware/main/nos_main.c:882-922`
- Purpose: USB serial command parser plus `ble_xfer_poll()`.
- Runs: priority 4, unpinned, 6144 B. `usb_serial_jtag_read_bytes` with 50 ms timeout, so a wakeup about 20 Hz in every state. Not display-gated.

### task-board-sense `firmware/main/board.c:967-1036`
- Purpose: IMU sampling hub.
- Runs: priority 2, unpinned, 4096 B internal static stack, 20 ms cycle, all states.
- Cadence: IMU reads at 0, 8, 16 ms of each cycle (two extra `knock_poll` reads), about 150 reads/s. Feeds `pedometer_update`, `sports_imu`, `workout_motion_sample`, `wrist_detect_feed`, `knock_feed`. `hal_esp32_sample_power()` (battery ADC, 8 samples) every 1 s (`firmware/main/hal_esp32.c:316-376`).
- Not display-gated. Stopped only by `board_prepare_sleep()` (`board.c:1126-1173`).

### task-board-fs `firmware/main/board.c:919-966`
- Purpose: slow housekeeping: `retry_devices` (437-500), SD remount retry, `rtc_maintain`, pedometer save to NVS "pedometer" every 60 s, CSV board log `/sdcard/watch/logs/*.csv` every 60 s (rotates at 1 MiB, `log_sample` 713-750).
- Runs: priority 1, 4096 B, 100 ms loop, all states.
- Gate: the pedometer save runs only when `s_background_persistence` is set (`board.c:45-47`), which `nos_main.c` sets in AOD/SENSOR/OFF and clears in ON.

### task-sports-sd `nos/sports.c:282-323`
- Purpose: drains the sports journal to SD, calls `diag_log_service`.
- Runs: priority 1, unpinned, PSRAM stack, about once per second. Not display-gated.

### task-workout-sd `nos/workout.c:761-783`
- Purpose: workout storage writer (`workout_storage_service`).
- Runs: priority 2, 500 ms; checkpoint every 30 s (`CHECKPOINT_MS`), sample `SAMPLE_MS=1000`, `HR_FRESH_MS=3000`. Not display-gated. Started by `writer_ensure`.

### task-track-save / task-memo-scan
- `track_save` (priority 1) is a one-shot on stop; `memo_scan` (priority 1) is on demand. Neither runs periodically.

### task-imu-rec (imu-sample prio 3, imu-write prio 1, timer-imu-rate) `firmware/main/imu_hal.c`
- Purpose: IMU recorder at 25/50/100 Hz, only while a recording is armed. Blocks power-off (`shutdown_guard`).

### task-ext `firmware/main/ext/ext_sys.c:144-167`
- Purpose: one worker for gps, lte, env, light, mag, hr.
- Runs: priority 0, unpinned, 4096 B PSRAM stack, wait capped at 50 ms. Not display-gated.
- Per module: HR background sample every `BG_EVERY_MS` (10 min) for up to `BG_MAX_MS` (40 s) (`ext_hr.c:230-310`); env forced read every `PERIOD_MS` 2000 (`ext_env.c`); light status every 400 ms (`ext_light.c`); gps/lte/mag/hr streams are lease-driven (`NOS_EXT_IDLE_MS` 8000 since the last getter call, `nos/hal_ext.h`); mag 20 Hz while leased.
- Shutdown: `hal_ext_shutdown_begin/poll` parks every module, 30 s timeout (`ext_sys.c:80-107`).

### task-exta / task-extf, timer-haptic
- Audio worker (`exta` priority 3, `extf` priority 0) and a one-shot haptic esp_timer chain. Active only while sound or haptics play.

### task-nimble-host `firmware/main/hal_esp32.c:1316-1410`
- Purpose: BLE host. Core 0, 4096 B. 100 ms control callout; fills the 8192 B NUS RX ring (`NUS_RING`, `hal_esp32.c:1070-1181`). Advertises with default parameters, connection update 80-120 ms, latency 3, 8 s timeout. Not display-gated; BLE on/off follows `nos_prefs.ble` via `radio_preferences`.

### task-weather `firmware/main/weather_service.c:88-141`
- Purpose: Open-Meteo fetch. Priority 2, 8192 B stack, 1 s loop; fetches every 1800 s on success or 60 s on failure, only while Wi-Fi is UP. Not display-gated.

### timer-wifi (wifi_retry, wifi_ap, airhop) `firmware/main/hal_esp32.c:1855-1955`
- `wifi_retry` backoff 2 s up to 300 s; `wifi_ap` 600 s window; `airhop`. Wi-Fi defaults to off (`wifi=0`). No `esp_wifi_set_ps` call anywhere; `CONFIG_ESP_COEX_POWER_MANAGEMENT` is off.

### timer-frame-pace `firmware/main/nos_main.c` (`s_frame_timer`, `pace_cb`)
- ON only. Armed through `nos_frame_wait_until` (`firmware/main/nos_frame_wait.c`) which clears and re-arms the timer each frame.

### svc-tick (services hub) `nos/svc.c:1303-1329`, `nos/apps/registry.c:62-79`
- `services_tick = svc_tick + background_tick`; `nos_bg_tick = services_tick`. Called by `nos_frame_run` (`nos/nos.c` about 2341, non-readonly only) and directly by the OFF branch.
- Sub-ticks and cadence:

| Sub-tick | Source | Cadence | Notes |
|---|---|---|---|
| `svc_track_tick` | `nos/svc_track.c:97-135` | per `nos_frame` | keeps GPS lease alive (`hal_gps_enable(1)`, `hal_gps_get`) |
| `svc_memo_tick`, `svc_cell_tick` | `nos/svc.c` | per tick | recorder and modem state |
| `phone_tick` | `nos/svc.c:1215-1254` | per tick, drains at most `LINES_PER_FRAME`=8 lines (`svc.c:49`) | battery status to phone every 10 min or 5 % or charger change |
| `companion_tick`, `music_tick` | `nos/svc.c:304` | per tick | |
| `wx_tick` | `nos/svc.c:412` | 10 s | |
| `sys_tick`, `act_tick`, `sports_tick` | `nos/svc.c:529-561`, `nos/sports.c:366` | 1 s gate (`s_slow_ms`) | `act_tick` saves "nosact" every 10 min, 5 s retry |
| `workout_tick` | `nos/workout.c:799-811` | per tick | |
| `heart_tick` | `nos/svc.c:586` | 60 s | |
| `timer_tick` | `nos/svc.c:894-910` | per tick | |
| `alarm_tick` | `nos/svc.c:826-853` | per tick, minute key `s_alarm_key` | |
| `call_find_tick` | `nos/svc.c:975-983` | per tick | |
| `ringer_tick` | `nos/svc.c:688-705` | per tick, 2 s repeat | haptic and sfx |
| `background_tick` | `nos/background.c` | 1 s gate, 13 jobs | steps, calories, battery, journal, trace, IMU rec, workout |

### eng-power-dfs `firmware/main/nos_power.c`
- `esp_pm_configure` min 80 / max 240 MHz, `light_sleep_enable=false` (always). Two PM locks: "nos-ui" (`ESP_PM_CPU_FREQ_MAX`, `nos_power_active_begin/end`) and "nos-prewarm" (`s_bake`, `nos_power_bake_begin/end`). `nos_power_set_max_mhz(80|240)` via `cpu_mode()` (`nos_main.c:634-646`). `CONFIG_PM_ENABLE=y`, `PM_DFS_INIT_AUTO` off, FreeRTOS tick 1000 Hz, no tickless idle (`firmware/sdkconfig:1917,1941`).

### eng-power-policy `nos/power_policy.h`
- Pure functions: `power_low_update` (enter <=15 %, leave >=20 %, external power clears), `power_idle_target(low, sensor_idle, aod)`, `power_wake_requested(low, touch, button, raised, knock) = touch || button || (!low && (raised || knock))`, `power_timeout_due`. Parallel `NOS_PWR_*` enum.

### eng-wake `core/wake_service.c`, `core/motion_wake.c`
- `wake_service_tick(nowms, eligible)` is called every loop pass (about 50 Hz) in all states; `eligible = s_pwr != PWR_ON && nos_prefs.raise_wake && !nos_locked && !s_low_battery`. `motion_wake_update` is a pickup-and-turn model that needs IMU samples younger than 150 ms; stages DISABLED, WAITING, ARMED, MOVING, COOLDOWN (2 s). Config in NVS "wake_cfg" (magic 0x57414b31). Knock double-tap (`knock_tick`, `nos/nos.c` about 2330) also needs `!low`.

### app-wrist-tuning / eng-wrist-detect `nos/wrist_detect.c`
- Fed from `board-sense` through `wrist_detect_feed`; capture window 4 s, quiet 600 ms. Active only after `wrist_detect_begin` (the Wrist tuning screen, which is SCR_KEEPAWAKE).

### hw-panel-ctl `firmware/main/nos_main.c:710-711`, `firmware/main/board.c`
- `hal_display_power` / `hal_display_mode`; panel sleep is DCS 0x28 / 0x10, wake 0x11, 100 ms, 0x29; brightness DCS 0x51. TE is not routed on B, so there is no TE ISR cost in any state.

## How it runs

### Run matrix

Legend: Y = runs normally; r = runs at reduced rate; - = not running.

| Component | ACTIVE (PWR_ON) | SENSOR (PWR_SENSOR) | AOD (PWR_AOD) | OFF (PWR_OFF) | Deep sleep (PWR_OFF via POWER OFF) |
|---|---|---|---|---|---|
| UI loop wakeups | 60 Hz paced by esp_timer | 50 Hz (20 ms delay) | 50 Hz | 50 Hz | - |
| Frame render | every frame | 1 per s | 1 per s | none | - |
| `nos_bg_tick` / `svc_tick` | every frame (dt=frame) | 1 per s, with the frame | 1 per s, with the frame | 1 per s, dt=1.0 | - |
| Touch poll | 10 ms | 40 ms | 40 ms | 40 ms (samples dropped) | - |
| `wake_service_tick` | Y (not eligible) | Y | Y | Y | - |
| `board-sense` IMU | 20 ms cycle, about 150 reads/s | same | same | same | stopped (`board_prepare_sleep`) |
| `board-fs` | 100 ms; pedometer NVS save suppressed | + pedometer save 60 s | same | same | stopped |
| `cmd` task | 50 ms | 50 ms | 50 ms | 50 ms | - |
| `ext` task | Y | Y | Y | Y | parked, then halted |
| NimBLE host and NUS ring | Y | Y | Y | Y | - |
| Phone line drain | 8 lines per frame (about 480/s at 60 fps) | 8 lines per s | 8 lines per s | 8 lines per s | - |
| Weather task | Y (Wi-Fi up) | Y | Y | Y | - |
| Sports/workout/track recording | Y | Y | Y | Y | blocked by `shutdown_guard` |
| Auto-brightness | 500 ms | - | - (`aod_level()`) | - | - |
| CPU max freq | 240 (lock held only during active work) | 80 | 240 on entry then policy | 80 | - |
| Light sleep | never | never | never | never | - |

### Questions

1. UI task spin rate per state. ON: one pass per presented frame (esp_timer-paced, target 60). SENSOR, AOD, OFF: a 20 ms `vTaskDelay`, so about 50 passes per second, of which rendering occurs once per second (SENSOR/AOD) or never (OFF); OFF still runs services once per second.
2. Touch polled in screen-off? Yes. `touch_task` runs in every state at 40 ms outside ON. Samples are dropped by `touch_drain(0)` unless they wake the display; the waking touch is swallowed.
3. DFS lock holders. Only the UI task takes "nos-ui" (`nos_power_active_begin` at the top of the loop pass, `nos_power_active_end` after `frame_drain`; in AOD/SENSOR it is released before the delay) and `dotbake` takes "nos-prewarm" for the boot prebake. No other task (touch, board-sense, ext, workout, sports, BLE, Wi-Fi) holds a lock, so with the UI idle the CPU sits at 80 MHz even while they work. OFF additionally caps max at 80.
4. What stays awake for sports, workout and track. Nothing special holds the display or CPU: sports and workout screens are not SCR_KEEPAWAKE, so the screen times out normally. Recording continues because `workout_tick`, `svc_track_tick` (GPS lease) and `sports_tick` are inside `svc_tick`, `board-sense` feeds `workout_motion_sample` and `sports_imu`, and the `workout_sd` / `sports_sd` writers and the `ext` task keep running. Screen-off reduces them to 1 Hz service ticks. They freeze while `mutate=0` (OTA writer admission or shutdown phase 3).
5. Deep sleep. Entered only by `nos_host_power(2)`: `shutdown_guard` (blocks on OTA busy, voice chat, IMU recording, audio record, workout running/paused/unsaved, storage dirty/uncertain, track active/pending/saving), then `sports_quiesce`, `go_state(PWR_ON)`, `prefs_save`, `hal_asset_cancel`; per-loop `shutdown_tick` runs `prefs_flush`, `hal_ext_shutdown_begin/poll`, `board_prepare_sleep` (stop board workers, final pedometer/RTC/sports flush, IMU CTRL7=0, SD unmount), `set_level(0)`, `panel_sleep(1)`, `frame_drain`, `vTaskDelay(150)`, then `esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0)` and `esp_deep_sleep_start()` (`nos_main.c:731-812`). Only wake source: ext0 on GPIO0 (BOOT, low). Wake is a full cold boot. No RTC alarm (GPIO15) or IMU interrupt (GPIO8) wake; `PIN_RTC_INT` and `PIN_IMU_INT` (`firmware/main/pins.h:55-56`) are not referenced anywhere. `esp_sleep_get_wakeup_cause`, `rtc_gpio` and `gpio_hold` are never used.
6. Alarm while in PWR_OFF. If the loop is in PWR_OFF (not deep sleep) `alarm_tick` still runs within the 1 Hz service tick; `ring_screen` pushes a SCR_KEEPAWAKE screen, which forces `go_state(PWR_ON)` within about 1 s and `ringer_tick` starts haptic and sound in the same tick (`nos/svc.c:659-705,826-853`). If the watch was powered off with POWER OFF (deep sleep), the alarm cannot fire; after the next boot only an alarm whose minute equals the boot minute rings (`s_alarm_key` starts at -1, `svc.c:90`); there is no missed-alarm catch-up. Alarm definitions persist ("nosalarm"); timer, stopwatch and snooze are RAM-only and lost.

### Notes on wake and notification behaviour
- Wake sources while screen-off: touch, button, raise (`raise_wake`), knock (`knock_lock`), SCR_KEEPAWAKE push (call, alarm ring, timer ring, find-watch, chat). Low battery disables raise and knock only.
- Phone notifications: `nos_banner` calls `nos_feedback(FB_NOTIFY)` (haptic and sound) and `nos_keep_awake`, but do not turn the panel on while it is OFF or SENSOR.
- Simulator note: `sim/nos_hal_sim.c` `hal_set_aod` is a no-op; the simulator does not model these states.

## Data in / data out

| Producer | Consumer | Data | Mechanism |
|---|---|---|---|
| `touch_task` | `app_main` | touch samples | 16-entry ring, `touch_drain` |
| `board-sense` | wake, pedometer, sports, workout, wrist, knock | IMU samples | direct calls per 20 ms cycle |
| NimBLE host | `phone_tick` | phone lines | 8192 B NUS ring, 8 lines per tick |
| `app_main` OFF branch | `svc_tick`, `background_tick` | 1 s ticks | direct call under `mutate` |
| `alarm_tick`, `timer_tick`, `call_find_tick` | `nos_push` | ring screen | SCR_KEEPAWAKE keeps display ON |
| `app_main` | `board.c` | `board_set_background_persistence` | flag |
| `app_main` | `hal_esp32.c` | `hal_battery_set_awake` | +25 mV awake, +10 mV otherwise (`hal_esp32.c:309,361`) |

## Persistence

| Item | Location | Screen-off behaviour |
|---|---|---|
| Prefs | SD `/watch/settings.0/1.bin`, NVS "nosprefs" mirror | `prefs_tick` runs in OFF/AOD/SENSOR with persistence allowed |
| Pedometer | NVS "pedometer" | 60 s from `board-fs`, only while `s_background_persistence`; final flush in `board_prepare_sleep` |
| Activity | NVS "nosact" | 10 min, 5 s retry (`act_tick`) |
| Alarms | NVS "nosalarm" | survive deep sleep; ringing does not |
| Wake config | NVS "wake_cfg" | persistent |
| Battery cal | NVS "bat_cal" | persistent |
| Workout | SD journal, 30 s checkpoint | continues in OFF |
| Board log | `/sdcard/watch/logs/*.csv` | 60 s, rotates at 1 MiB |
| Timer, stopwatch, snooze | RAM | lost in deep sleep |

## Interactions

- [[Power and battery]]: this note is the per-state breakdown of its policy and DFS sections.
- [[Engine and scheduling]]: `nos_frame_run`, `nos_bg_tick` and the `mutate` admission gate.
- [[Rendering pipeline]]: flush task, frame drain and panel sleep.
- [[Services hub]]: `svc_tick` sub-ticks.
- [[Health and activity]], [[Sports and workouts]], [[Calorie trace]]: recording continues through service ticks.
- [[Storage and persistence]]: SD and NVS flush paths before deep sleep.
- [[Hardware and drivers]]: `board.c`, `ext_sys.c`, IMU and panel control.
- [[Connectivity and voice]]: BLE, Wi-Fi, weather and chat SCR_KEEPAWAKE.
- [[Shell and faces]], [[Utility apps]], [[Settings and developer]]: alarm and timer apps, the sensor-idle toggle.
- [[Gaps]], [[OS overview]].

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| High | Default `sensor_idle=1` overrides AOD and OFF; the screen never reaches OFF on timeout, and the toggle is hidden in Battery History. | `nos/prefs.c:140-163`, `nos/power_policy.h`, `nos/apps/fit_energy.c:155,247` | Users who disable AOD still sit in the SENSOR path. |
| High | No light sleep, tickless idle or wake interrupt; screen-off is a 20 ms polling loop and the IMU is read about 150 times/s. DFS is the only saving. | `firmware/main/nos_power.c`, `firmware/sdkconfig:1917,1941`, `firmware/main/board.c:967-1036` | Large idle drain (see [[Power and battery]]). |
| Medium | Deep sleep wakes only on GPIO0; RTC INT (GPIO15) and IMU INT (GPIO8) unused; alarms never fire when powered off and are not reported later. | `firmware/main/nos_main.c:810-811`, `firmware/main/pins.h:55-56`, `nos/svc.c:90,826-853` | A powered-off watch misses alarms silently. |
| Medium | Phone line drain is 8 lines per `svc_tick`; in OFF/AOD/SENSOR that is 8 lines/s against an 8192 B ring; overflow drops lines. Call and find-phone notice latency about 1 s in OFF. | `nos/svc.c:49,1236`, `firmware/main/hal_esp32.c:1070-1181` | Message bursts lost while screen-off. |
| Medium | `svc_tick` freezes when `mutate=0` (OTA writer, shutdown phase 3): alarms, workouts, track and phone parsing stall. | `nos/nos.c` about 2341, `firmware/main/nos_main.c` | Alarm or recording gap during OTA. |
| Low | Pedometer NVS save is suppressed while ON, so continuous screen-on use only checkpoints at shutdown. | `firmware/main/board.c:45-47,919-966` | Steps lost on a crash during long screen-on use. |
| Low | The `svc_tick` 1 s slow gate can alias with the OFF loop's 1000 ms gate and occasionally skip a second of sys/act/sports ticks. | `nos/svc.c:1303-1329`, `firmware/main/nos_main.c:2091-2103` | Rare small timing jitter. |
| Low | Notifications in OFF/SENSOR buzz but never wake the display. | `nos/nos.c:509-521,557-573` | Haptic-only until the user touches or raises. |
| Low | Ext modules (env 2 s, light 400 ms, HR background every 10 min) are not display-gated. | `firmware/main/ext/ext_env.c`, `ext_light.c`, `ext_hr.c` | Extra drain when the strap is fitted. |
| Low | No deep-sleep GPIO hold; peripheral pin state (LCD enable GPIO42, etc.) is undefined. `SP_DEEP_SLEEP` is never emitted. | `firmware/main/nos_main.c:810-811` | Possible leakage; unverified on hardware. |
