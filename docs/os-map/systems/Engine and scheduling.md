---
title: "Engine and scheduling"
tags: [nos, os-map]
updated: 2026-10-23
---

# Engine and scheduling

## Summary

NOS on the 1.43-B is a single cooperative UI task (`app_main`, core 0, FreeRTOS priority 1) that owns the frame loop in `firmware/main/nos_main.c` (loop body ~1977-2290). Each ON-state iteration drains serial commands and touch samples, calls `nos_frame_run()` (`nos/nos.c:2299-2509`, which runs input recognition, `nos_bg_tick` = `services_tick` = `svc_tick` + `background_tick`, transition and screen ticks, damage computation, and stripe rendering), waits for DMA, then paces with an `esp_timer` ("pace") + binary semaphore. There is no RTOS-level scheduler for apps: all screens and services tick from this one loop. Worker tasks (flush, touch, cmd, board-sense, board-fs, IMU, SD writers, radios, weather, voice, ext) run beside it.

- Animation clock is 40 fps (`NOS_FPS`, `nos/render_config.h`), presentation target 60 fps (`NOS_PRESENT_FPS`), period 25 ms; dt is bounded to 4 periods; presentation rate is capped by pref `fps_cap` (0/24/30/40/48/60, `nos/frame_cap.h`).
- Screen stack is 8 deep (`MAX_DEPTH`), with transitions TX_NONE/PUSH/RISE/DROP/IRIS/DISSOLVE and a finger-owned back-swipe.
- Input: FT3168 touch (I2C polled by the `touch` task), one GPIO0 button (ISR), serial `btn` pulse, APDS-9960 air gestures (strap, only music consumes). There is NO crown in code.
- Screen off: the UI task keeps running at 50 Hz (20 ms delay) and ticks services once per second with dt=1.0 (OFF) or paints at 1 Hz with dt<=0.1 (AOD/SENSOR). Light sleep is disabled (`nos_power.c`).

## Components

### eng-frame-loop - main UI task
- Purpose: boot, power-state machine, command execution, frame pacing. `firmware/main/nos_main.c:1899` (`app_main`), loop ~1977-2290.
- Runs: task "main" core 0, prio 1 (`ESP_TASK_MAIN_PRIO`), stack 8192 (`firmware/sdkconfig`). 60 Hz ON (`nos_frame_pacer_capped` + `nos_frame_wait_until`), 50 Hz in OFF/AOD/SENSOR (`vTaskDelay(20 ms)`).
- Reads: `s_cmd_q`, touch ring `s_tring`, button flags, `nos_prefs`, battery/power state, wake_service.
- Writes: panel brightness/sleep (via flush queue), `nos_prefs` persistence (`prefs_tick`), `nos_t`, `nos_frame`.
- Boot order: `nos_frame_priority_init`, `nos_heap_diag_init`, DMA stripe buffers (2x466x16 + 2x466x8 rotated) and PSRAM scratch, `nos_power_init`, `hal_esp32_init_periph`, `lcd_init`, `boot_resources_init` (nos_main.c:1885-1891 creates pace timer, flush/touch/cmd parked via `boot_worker_task`), `board_init`, `diag_log_init`, `settings_load`, `prefs_load`, `hal_ext_init`, `voice_chat_init`, radios, GPIO0 ISR, `nos_init`, `nos_apps_boot`, dotbake (1950), `wake_service_init`, release touch/cmd, `nos_usb_diag_start`, print `READY`/`NOSIDENTITY`.

### eng-nos-frame - `nos_frame_run` / `frame_run`
- `nos/nos.c:2299-2509`. Order: `nos_dev_frame` (unless readonly) -> dt -> `nos_frame++` -> `hal_frame_begin` (freezes hal_millis/datetime) -> `knock_tick` -> `process_touch` (885) -> `process_long` (IN_LONG at 450 ms of nos_t) -> `process_button` (959) -> IN_AIR -> `nos_bg_tick(dt)` -> `tick_transition` (2167) -> `tick_screens` (both screens during a transition) -> `tick_overlays` (banner/toast) -> `prefs_tick` -> scene record (skipped during moving transition) -> `compute_damage` -> stripe align/merge -> `render_rect` per stripe (`s_wait_free`, `scene_draw` 1784, `s_flush`) -> `hal_frame_end` -> stats in `nos_last`.
- `nos_t` (double, `nos/animation_time.h`) is not advanced in readonly mode or while the screen is OFF.

### eng-screen-stack - stack, push/pop, transitions
- `nos/nos.c`: `s_stack[MAX_DEPTH=8]` of {screen, tx}. API: `nos_push` (303), `nos_pop`, `nos_pop_with`, `nos_replace`, `nos_retarget`, `nos_home`, `nos_home_now`, `nos_pull` (466), `nos_set_root`. Overflow in `nos_push` is a silent reject (logs DIAG_RESULT_REJECTED, nos.c:305).
- Transition struct `T` with states IDLE/AUTO/FINGER; types NONE/PUSH/RISE/DROP/IRIS/DISSOLVE; variants LEGACY/STEADY/AXIS/CUT (PUSH->AXIS, RISE/DROP->STEADY by default); AXIS_FRAMES=10, TX_PEAK_PX 62, iris 12-16 frames. `tick_transition` 2167, `tx_complete` 2115.
- Screen flags (nos/nos.h): SCR_HDRAG, SCR_NOBACK, SCR_OPAQUE_BG, SCR_KEEPAWAKE, SCR_NATIVE_CACHE, SCR_LIVE_RENDER. Navigation is logged via `diag_action_*`.
- Banner/toast: `nos_banner` (509) queues, buzzes, calls `nos_keep_awake()`; timers on `nos_t`.

### eng-input - input routing
- Touch: `touch_task` (nos_main.c, core 1, prio 5) polls FT3168 over I2C (`board_touch_read` board.c:342-386) every 10 ms when `s_touch_fast`, else 40 ms. `nos_touch_contact.h` bridges invalid samples (150 ms hold then cancel). 16-entry ring `s_tring`; `touch_drain` feeds `nos_touch_at()` just before `nos_frame_run`. A waking touch is swallowed.
- Recogniser (nos.c ~800-1000): SLOP 12 px, 12-sample least-squares velocity (VMAX 6000), prediction 16 ms capped 24 px. Owners 0 screen / 1 engine (back-swipe, pull) / 2 banner / 3 spent. Back-swipe = rightward drag on any pushed screen without SCR_NOBACK (left-edge 44 px if SCR_HDRAG).
- Button: GPIO0 ISR `btn_isr` (25 ms debounce) + serial `btn` pulse (80 ms, `nos_button_pulse.h`). `process_button` -> IN_BTN on release, IN_BTN_DOUBLE (2nd press within 320 ms), IN_BTN_LONG (600 ms). Unhandled IN_BTN with depth>1: FB_BACK + `nos_home`; unhandled IN_BTN_LONG: push `nos_power_screen` (TX_RISE). Home handles IN_BTN (face <-> apps page) and IN_BTN_DOUBLE (reopen `nos_last_app()`).
- Air gestures: `nos_air()` only called from `nos/apps/con_music.c:433`; single-slot `s_air`.
- Knock-to-lock: `hal_knock_count` -> `nos_set_locked`.
- Crown: none. Only matches are a heart-rate drawing comment and a haptic comment.

### eng-services-tick - per-frame services
- `nos/apps/registry.c` sets `nos_bg_tick = services_tick` -> `svc_tick(dt)` (`nos/svc.c:1303`) then `background_tick()` (`nos/background.c:160`).
- svc_tick every call: `svc_track_tick`, `svc_memo_tick`, `svc_cell_tick`, `phone_tick`, `companion_tick`, `music_tick`, `workout_tick`, `wx_tick`, stopwatch elapsed, `timer_tick`, `alarm_tick`, `call_find_tick`, `ringer_tick`. Once per second: `sys_tick`, `act_tick`, `sports_tick`. Every 60 s: `heart_tick`.
- background_tick: 1 Hz `sample_services` into 120-point histories; 13 jobs (MOTION, STEPS, CALORIES, BATTERY, JOURNAL, TRACE, IMU_REC, WORKOUT, HEART, GPS, CLIMATE, LIGHT, COMPASS).
- `svc_track_tick` once-per-frame guard keys on `nos_frame` (`nos/svc_track.c:103`), hence the manual `nos_frame++` in the OFF path.

### sys-app-registry
- `nos/apps/registry.c`: 28 launcher entries (Activity, Sports, Energy, Jobs, Chat, Battery history, Sensor logs, Wrist tuning, Quick report, Heart rate, Breathe, Weather, Music, Timer, Stopwatch, Alarms, World clock, Compass, IMU, Barometer, Navigate, Cellular, Recorder, Find phone, Torch, Developer tools, Diagnostics, Settings). `nos_open` records `s_last_app` (RAM only, registry.c:41), sets iris origin, plays FB_OPEN, `nos_push(s, TX_IRIS)`. `nos_apps_boot` does `svc_init`, `background_init`, sets root `scr_home`, pushes `scr_boot`. Serial `open NAME` resolves via `screen_by_name`.

### eng-frame-pacer
- `firmware/main/nos_frame_wait.c/.h`, `nos_frame_pacer.h`: one-shot esp_timer "pace" + semaphore `s_frame_tick`, timeout 2*period+2 ms; on fault resets pacer and `vTaskDelay(1)`. `nos_frame_priority.h`: mode 3 experiment raises paint priority to 3; default 0.

### task-flush - display flush
- nos_main.c:1887 `xTaskCreatePinnedToCore(...,"flush",3072,...,4,...,1)`. Queue depth 3; `s_buf_free`/`s_dma_ready` counting semaphores of 2; jobs STRIPE/BRIGHT/INVERT/SLEEP/FENCE/EXPOSURE_FENCE. Rotates each stripe CCW (`board_pixels_ccw_swap`), calls `nos_display_te_wait` (no-op on B), then `esp_lcd_panel_draw_bitmap(y0+offset, 466-x0-w, ..., 466-x0)`. `on_dma_done` IRAM ISR. Any 1500 ms semaphore/fence timeout calls `display_fault` -> RTC log + `esp_restart()` (nos_main.c:130-148).

### task-touch
- nos_main.c:1889, core 1, prio 5, stack `TOUCH_STACK_BYTES` (4096). `vTaskDelayUntil` 10 ms ON / 40 ms otherwise.

### task-cmd - serial command task
- nos_main.c:1891, unpinned, prio 4, stack 6144. Polls `ble_xfer_poll()` and USB bytes with 50 ms timeout; `nos/command_input.c` (192-byte cap, control-char reject) and `nos/host_request.c` (`req <8hex> <cmd>` envelope). SD transfer verbs (sdfeatures/sdls/sdget/sdget2/sdput/sdput!/sdcrc/sdrm) run directly in this task under `s_host_reply_lock` (3 s); other commands go to queue `s_cmd_q` (depth 4) and execute in the UI loop (`cmd_exec`). During OTA / shutdown phase 3 only a read-only allowlist runs. "Quiet" commands do not wake the display.

### task-dotbake
- nos_main.c:1950 core 1 prio 1 stack 3072; one-shot `dot_prebake_all` + orrery basis/shade checks, then self-deletes. Holds PM lock `nos-prewarm`.

### task-board-sense
- board.c:1036 static, unpinned, prio 2, 4096. 20 ms cycle: IMU, pedometer, knock polls at +8/+16 ms, 1 Hz power sample.

### task-board-fs
- board.c:1042 (`board_fs_task` 919) prio 1, 4096, internal caps. 100 ms loop: pedometer save every 60 s (5 s retry), log sample 60 s, `rtc_maintain`, device retry, SD retry.

### task-imu-sample / task-imu-write / timer-imu-rate
- `firmware/main/imu_hal.c:64` creates static unpinned tasks. `imu-sample` prio 3, 4096, woken by esp_timer `imu-rate` (periodic 1e6/hz, `skip_unhandled_events=false`, imu_hal.c:85) while streaming. `imu-write` prio 1, 6144, 20 ms notify wait while recording else 1000 ms.

### task-sports_sd / task-workout_sd / task-track_save / task-memo_scan
- `nos/sports.c:323` `sports_sd` prio 1 4096 PSRAM, 1 s drain (20 ms while a query is pending), also runs `diag_log_service()`. `nos/workout.c:777` `workout_sd` prio 2 6144 PSRAM, 500 ms. `nos/svc_track.c:299` `track_save` prio 1 4096 one-shot. `nos/apps/con_memo.c:253` `memo_scan` prio 1 4096 one-shot.

### task-asset-read
- `hal_esp32.c:2490` static, prio 1, notify-driven, `vTaskDelay(1)` between SD steps.

### task-wifi-settings + timers wifi_retry / wifi_ap / airhop
- `hal_esp32.c:2268` prio 2, 6144, 100 ms queue wait, `air_supervise`. esp_timers: `wifi_retry` (1869, one-shot backoff), `wifi_ap` (1920, one-shot 600 s), `airhop` (2179, periodic `AIR_DWELL_MS`).

### task-nimble_host
- `hal_esp32.c:1531`, core 0, prio `configMAX_PRIORITIES-4` = 21, `nimble_port_run`.

### task-weather
- `firmware/main/weather_service.c:175` prio 2, 8192. Polls 1 s; fetch needs Wi-Fi up, valid location and epoch; 10 s minimum between attempts; next due 1800 s on success, 60 s on failure.

### task-voice
- `voice_chat.c:519` prio 3, 10240 PSRAM, created on first use. On B `voice_audio_open()` returns 0 (`voice_audio.c:116`) and `view.available` is set only under `CONFIG_WATCH_BOARD_143C` (`voice_chat.c:113-115`); `nos/apps/con_chat.c:131` shows "Voice chat needs the C board mic and speaker." So Chat is a launcher entry that only shows an unavailable message on B.

### task-ota-upload / task-ota_url
- `ota.c:303` `ota-upload` prio 5 4096; `ota.c:387` `ota_url` prio 5 8192. On demand. `ota_mark_valid` is called after 90 good frames when mutating (nos_main.c ~2227).

### task-ext / task-exta / task-extf / timer-haptic
- `ext/ext_sys.c:186` `ext` prio 0, 4096 PSRAM, delay <=50 ms, drives gps/lte/env/light/mag/hr `_service`. `ext/ext_audio.c:344` `exta` prio 3 (notify-blocked when idle), `:350` `extf` prio 0. `ext/ext_haptic.c:104` esp_timer `haptic` one-shot per segment. Strap pins compiled into B (`ext_priv.h:27-44`).

### timer-pace
- nos_main.c:1885 one-shot esp_timer "pace" -> `pace_cb` gives `s_frame_tick`.

### hw-isrs
- `btn_isr` (GPIO0, 25 ms debounce); `on_dma_done` (IRAM). `touch_isr`, `te_isr` and TE paths are C-only/no-ops on B (pins.h has no TE or touch INT/RST).

### sys-power-gate - power states
- `nos_main.c` ~2040-2130, `firmware/main/nos_power.c`, `nos/power_policy.h`. States PWR_ON/AOD/SENSOR/OFF. `power_idle_target`: OFF if low battery, SENSOR if `sensor_idle`, AOD if `aod`, else OFF. Timeout `nos_prefs.sleep_s`; SCR_KEEPAWAKE or finger-down blocks sleep; `s_no_sleep` only via serial `awake`. Low-battery latch: enter <=15%, exit >=20%. PM locks `nos-ui` (held during active work) and `nos-prewarm`; `cpu_mode` 80/240 MHz.
- Shutdown (`nos_host_power`, `shutdown_tick`): guard blocks on OTA, IMU recording, workout/track save, audio; phases prefs flush -> `hal_ext_shutdown_begin/poll` -> `board_prepare_sleep` -> panel sleep + `frame_drain` -> restart or `esp_deep_sleep_start` with ext0 on GPIO0.

## How it runs

ON path per iteration: `nos_frame_priority_restore`; `nos_power_active_begin` (PM lock); `diag_log_tick`, `nos_heap_diag_tick` (1 Hz); OTA/mutate gating; `voice_chat_maintenance`, `hal_setup_poll`, prefs pollers, `wake_wrist_sync`, `shutdown_tick`; dequeue and run serial commands; button + low-battery update; `wake_service_tick`; knock; wake and sleep policy; `orrery_card_asset_poll`, `theme_apply`; `auto_brightness_tick` (500 ms); `touch_drain`; `nos_frame_run`; `frame_drain`; debounced `prefs_tick`; `set_level` brightness after the frame; pacing wait (or `vTaskDelay(1)` when uncapped and nothing painted).

Screen off:
- PWR_OFF: `cpu_mode(80)`; once per 1000 ms `nos_frame++; nos_bg_tick(1.0f)` (services tick, screens do not, `nos_t` frozen); if the top screen has SCR_KEEPAWAKE the state flips back to ON (call, alarm ring, timer ring, find-watch); `prefs_tick` with persistence allowed unless `voice_chat_active`; `touch_drain(0)` discards samples; 20 ms `vTaskDelay`.
- PWR_AOD/SENSOR: 1 Hz `nos_frame_run` (dt capped at 0.1 s so services see <=0.1 and `nos_t` runs 0.1x), `frame_drain`, `set_level(aod_level)`; SENSOR drops to 80 MHz; AOD entry ramp runs at full rate.
- Wake: touch, button, raise-to-wake (`wake_service`), knock; low battery blocks raise and knock.

Priority ladder: Wi-Fi (23, core 0) > esp_timer (22, core 0) > NimBLE (21, core 0) > lwIP tcpip (18) > ota (5) > touch (5, core 1) > flush/cmd (4) > imu-sample/exta/voice (3) > board-sense/wifi-settings/weather/workout_sd (2) > UI/main (1, core 0). Tick 1000 Hz, PM on (DFS 80-240), light sleep off, int WDT 800 ms, task WDT 5 s, coredump to flash (`firmware/sdkconfig`).

### Task and timer table (B build)

| Name | Core | Prio | Stack | Cadence | Source |
|---|---|---|---|---|---|
| main (app_main) | 0 | 1 | 8192 | 60 Hz ON; 50 Hz off | nos_main.c:1899 |
| flush | 1 | 4 | 3072 | queue-driven | nos_main.c:1887 |
| touch | 1 | 5 | 4096 | 10 ms ON / 40 ms | nos_main.c:1889 |
| cmd | any | 4 | 6144 | 50 ms USB wait | nos_main.c:1891 |
| dotbake | 1 | 1 | 3072 | one-shot at boot | nos_main.c:1950 |
| board-sense | any | 2 | 4096 | 20 ms | board.c:1036 |
| board-fs | any | 1 | 4096 | 100 ms | board.c:1042 |
| imu-sample | any | 3 | 4096 | imu-rate timer | imu_hal.c:64 |
| imu-write | any | 1 | 6144 | 20 ms / 1 s | imu_hal.c:64 |
| sports_sd | any | 1 | 4096 | 1 s / 20 ms | sports.c:323 |
| workout_sd | any | 2 | 6144 | 500 ms | workout.c:777 |
| track_save | any | 1 | 4096 | one-shot | svc_track.c:299 |
| memo_scan | any | 1 | 4096 | one-shot | con_memo.c:253 |
| asset-read | any | 1 | nos_mem | notify | hal_esp32.c:2490 |
| wifi-settings | any | 2 | 6144 | 100 ms | hal_esp32.c:2268 |
| nimble_host | 0 | 21 | 4096 | event loop | hal_esp32.c:1531 |
| weather | any | 2 | 8192 | 1 s poll | weather_service.c:175 |
| voice | any | 3 | 10240 | on first use | voice_chat.c:519 |
| ota-upload | any | 5 | 4096 | on demand | ota.c:303 |
| ota_url | any | 5 | 8192 | on demand | ota.c:387 |
| ext | any | 0 | 4096 | <=50 ms | ext_sys.c:186 |
| exta | any | 3 | 4096 | notify | ext_audio.c:344 |
| extf | any | 0 | 4096 | notify | ext_audio.c:350 |

esp_timers: `pace` (one-shot, nos_main.c:1885), `imu-rate` (periodic, imu_hal.c:85), `haptic` (one-shot chain, ext_haptic.c:104), `wifi_retry` (one-shot, hal_esp32.c:1869), `wifi_ap` (one-shot 600 s, hal_esp32.c:1920), `airhop` (periodic, hal_esp32.c:2179).

## Data in / data out

| In | From | Mechanism |
|---|---|---|
| Touch samples | `touch` task | 16-entry ring `s_tring` -> `touch_drain` |
| Button | GPIO0 ISR, serial `btn` | flags + `nos_button` |
| Serial commands | `cmd` task | queue `s_cmd_q` (depth 4) |
| Air gestures | music strap | `nos_air()` single slot |
| Wake events | `wake_service`, knock | `power_wake_requested` |
| Time | `hal_millis` (frozen per frame) | `hal_frame_begin/end` |

| Out | To | Mechanism |
|---|---|---|
| Stripes | `flush` task | queue + semaphores |
| Brightness/invert/sleep | `flush` task | panel jobs with ACK |
| Service ticks | svc/background | `nos_bg_tick(dt)` |
| Prefs | SD/NVS | `prefs_tick` debounce |
| Diag log | RTC RAM | `diag_log_*` |

## Persistence

`nos_prefs` (RAM, loaded at boot; persisted by `prefs_tick` at `nos/prefs.c:366`; per CLAUDE.md B settings live on SD with NVS fallback); retained RTC diagnostic log (`nos/diag_log.c`, 30 s power/heap samples); coredump flash partition; `s_last_app` is RAM only. The engine reaches SD only indirectly.

## Interactions

- [[Rendering pipeline]]: stripes, damage, DMA, panel jobs.
- [[Services hub]]: `svc_tick` and `background_tick`.
- [[Health and activity]], [[Sports and workouts]], [[Calorie trace]]: ticked from `svc_tick` and `background_tick`; workers sports_sd/workout_sd.
- [[Power and battery]]: PM locks, DFS, low-battery latch, sleep states.
- [[Storage and persistence]]: prefs_tick, board-fs, SD writers.
- [[Hardware and drivers]]: board-sense, IMU tasks, FT3168 polling, GPIO0.
- [[Connectivity and voice]]: NimBLE, Wi-Fi, weather, voice, cmd task transfers.
- [[Shell and faces]]: home screen, button routing, boot screen.
- [[Utility apps]], [[Settings and developer]]: registry apps, `nos_dev_frame`.
- [[Gaps]], [[OS overview]].

## Gaps

1. High: UI task is the lowest-priority task on core 0 (prio 1) beside Wi-Fi 23, esp_timer 22, NimBLE 21, lwIP 18 and unpinned prio 2-5 workers; radio or worker bursts stall frames. Evidence: `firmware/sdkconfig`, nos_main.c:1887-1891, hal_esp32.c:1531. The mode-3 priority experiment is off by default (`nos_frame_priority.h`).
2. Medium: Idle ON loop re-runs tick, record and compare at 60 Hz regardless of change (nos_main.c:2132-2290); battery cost.
3. Medium: OFF/AOD loop wakes at 50 Hz with light sleep disabled; touch task polls I2C at 25 Hz off-screen (no INT on B); cmd wakes at 20 Hz.
4. Medium: Clock inconsistency off-screen: OFF dt=1.0 with nos_t frozen, AOD dt<=0.1 with nos_t 0.1x; `nos_frame` bumped manually for guards (svc_track.c:103). Services tick only 1 Hz off.
5. Medium: Banners while the screen is OFF do not wake the display and use nos_t timers, so they appear at next wake, possibly stale; in AOD they draw ~40 s (nos.c:509-521, 1768; svc.c:232).
6. Low: Programmatic push/replace/home during a finger-owned gesture does not reset `P.owner`; a later release may pop the new screen (nos.c:303-325, 766-792, 948). Traced only, not benched.
7. Low: `nos_push` overflow and `nos_pull` fail silently (nos.c:305, 466).
8. Low: `nos_air` keeps one gesture per frame while con_music.c:433 polls four.
9. Medium: `hal_millis`/`hal_get_datetime` return frozen non-atomic values to every task during a UI frame (hal_esp32.c:65-66,111-146); long frames make other cores see stale time.
10. Low: Docs/comments mention a crown but none exists (only GPIO0 + touch).
11. Low: Stale comments: nos.h says KEEPAWAKE is for stopwatch, which does not set it.
12. Medium: Chat is listed in the launcher (registry.c:15) but unusable on B (voice_audio.c:116, voice_chat.c:113-115, con_chat.c:131).
13. Low: `ext` and `extf` run at prio 0 (ext_sys.c:186, ext_audio.c:350), starved by anything else.
14. High: Any 1500 ms display queue or fence timeout reboots the watch (nos_main.c:130-148).
15. Low: `display_te` pref and TE report remain although TE is compiled out on B (pins.h, nos_display_te.c).
16. Low: `s_last_app` is RAM only, so double-press reopen resets at reboot (registry.c:41).
17. Low: `nos_legacy.c` shim (6 globals) keeps dead legacy code linking; `core/anim.c` not verified used by NOS (NOS uses nos/motion.h).
18. Low: OTA validity mark only after 90 good frames when mutating (nos_main.c ~2227).
