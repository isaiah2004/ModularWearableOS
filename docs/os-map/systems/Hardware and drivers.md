---
title: "Hardware and drivers"
tags: [nos, os-map]
updated: 2026-10-23
---

# Hardware and drivers

## Summary

Scope: the Waveshare ESP32-S3-Touch-AMOLED-1.43-B (16 MB flash, 8 MB PSRAM) as built by `firmware/` with `WATCH_BOARD_NAME=ESP32-S3-Touch-AMOLED-1.43-B` and `CONFIG_WATCH_BOARD_143C` unset. Legacy `firmware/main/main.c`, `core/wos.c`, `core/apps/*`, `core/pip*.c` are not built and are only mentioned where they explain dead HAL entry points.

- **On-board chips (B):** FT3168 touch (I2C 0x38, polled, no reset or interrupt GPIO), QMI8658 IMU (0x6b), PCF85063 RTC (0x51), SD card (one-bit SDMMC), battery divider on GPIO4/ADC1 ch3, BOOT button GPIO0, CO5300-profile AMOLED on QSPI (owned by [[Rendering pipeline]]). There is **no built-in audio** and **no TE line** (`firmware/main/pins.h`). The IMU interrupt line is GPIO8 and the RTC interrupt is GPIO15, but both are only `#define`d: no driver configures either pin as an interrupt (`firmware/main/pins.h:55-56`; grep of `PIN_IMU_INT`/`PIN_RTC_INT` finds no other use). Everything is polled.
- **Strap modules (back header, optional):** coin ERM haptic (IO16), MAX98357A amp + INMP441 mic (I2S on IO1/2/3/5/6, so audio exists only on the strap), GPS (UART, IO7 RX / IO17 TX), Quectel EG800AK LTE (UART0 IO43/IO44, PWRKEY IO18), BME280 env (0x76/0x77), APDS-9960 light/proximity/gesture (0x39), QMC5883L/HMC5883L compass (0x0D/0x1E), MAX30102 HR/SpO2 (0x57). `firmware/main/ext/ext_priv.h:24-44` (compiled to "absent" when `CONFIG_WATCH_BOARD_143C`).
- **Pattern:** a driver/task samples a chip, publishes a small struct (or ring) into RAM under a spinlock/mutex, and `core/hal.h` / `nos/hal_ext.h` getters hand a copy to UI and services. Nothing in UI code touches I2C. The strap contract is `nos/hal_ext.h` (presence bits `EXT_*`, `NOS_EXT_IDLE_MS`=8000 auto-stop of every stream whose getter is not called).
- **Bus:** one I2C master port 0 (SDA47/SCL48) shared by touch, IMU, RTC and all strap I2C chips. The IDF master driver serializes transactions per bus; `board.c` adds per-device mutexes for touch, IMU and RTC, but strap drivers (`ext_*`) do not take those and nothing coordinates them beyond the IDF bus lock.
- **Detection:** all probing is at boot. On-board chips are probed in `board_init`/`configure_device` and retried only by an explicit `board_retry_devices`/the board-sense loop. Strap modules are probed once in `hal_ext_init` (I2C by device ID read; GPS/LTE by asynchronous UART probes). No hot-plug re-probe exists except `hal_gps_enable(1)` re-running the GPS probe when absent.
- **Biggest findings:** IMU is polled about 150 reads/s even though INT is wired; deep sleep wakes only on BOOT (ext0 GPIO0), so RTC alarms and raise-to-wake cannot wake from deep sleep; Control Center LTE/GPS toggles are cosmetic against the 8 s idle expiry; every outdoor workout re-triggers a 12.6 s GPS probe when no GPS is attached; `EXT_SPEAKER` is reported present unconditionally; the NOS build carries a dead battery log.

## Components

### Board bring-up and device layer (`board.c`, `board.h`, `board_protocol.c`)
- Purpose: owns the I2C bus, the FT3168 / QMI8658 / PCF85063 / SD devices, per-device mutexes, diagnostics, RTC clock restore/write-back, SD mount lease, sleep protocol, and the pure decode helpers (RTC BCD, touch packet, IMU raw to `hal_imu`, pixel rotation, `board_native_to_ui`).
- Where it runs: `board_init` (called from `hal_esp32_init_periph`, `firmware/main/hal_esp32.c:2312`) on the boot task; `board-sense` and `board-fs` tasks (below); touch reads in the `touch` task of `nos_main.c`.
- State: device ok flags, last result codes, retry counters (`firmware/main/board.c:49-92`); diagnostics API `board_device_diagnostics` (`:186`, results at `:179-224`).
- `configure_device` (`:408-436`) writes initial register sets (IMU ODR/range, RTC control); `retry_devices` (`:437-485`) re-runs it for any device that failed, rate limited.
- Pure helpers in `firmware/main/board_protocol.c`: RTC BCD codec (`:19-52`), touch decode (`:53`), `board_native_to_ui` (x=465-y, y=native_x; ~`:240`), IMU decode (`:261`). These are host-testable.

### FT3168 touch (hw-ft3168-touch)
- Driver: `touch_rd` (`board.c:244-259`), `board_touch_read` (`:342`/`:377`, two variants selected at compile time), decode in `board_protocol.c:53`. I2C 0x38, 400 kHz, no reset or interrupt GPIO, so there is no wake-on-touch; presence is detected by polling.
- Consumer: `touch` task in `firmware/main/nos_main.c:510-622` (priority 5, core 1, created `:1889`) polls and feeds the engine (see [[Engine and scheduling]]). Poll cadence is about 25 Hz when the screen is off (`nos_main.c:588`) and the frame rate while awake. Transform: native to UI is the inverse of the display CCW rotation (UI (x,y) maps to native (y,465-x); touch maps back to (465-y,x)).
- Absence: `board_touch_diagnostics` (`board.c:236`) exposes timeouts; `board_touch_inject_timeouts` (`:232`) is a test hook. Timeouts bump counters and retry; the UI stays alive without input.

### QMI8658 IMU (hw-qmi8658-imu) and board-sense task (task-board-sense)
- Driver: `imu_take` (`board.c:291-321`) reads the 14-byte sample under the IMU mutex; `board_imu_read` (`:388`) returns the latest cached, scaled `hal_imu`; `board_imu_raw` (`:398`); `board_imu_available` (`:397`). Exposed as `hal_imu_read` (`hal_esp32.c:477`).
- Task: `board_task` ("board-sense", 4096 B PSRAM stack, priority 2, `tskNO_AFFINITY`, `board.c:967-1027`, created `:1036`). 20 ms cycle: IMU read, `vTaskDelay(8)`, `knock_poll()`, `vTaskDelay(8)`, `knock_poll()`, `vTaskDelay(4)` (`:1021-1025`), i.e. about 150 IMU reads/s whether or not anyone consumes them. Once per second `hal_esp32_sample_power()` runs inside it (`:1018`). It also drives the step counter feed (pedometer) and wrist-motion/knock logic documented in [[Health and activity]] and [[Power and battery]].
- Interrupt unused: GPIO8 is the IMU INT, never configured (`pins.h:55`).

### IMU recording backend (task-imu, timer-imu-rate; `imu_hal.c`, `nos/imu_backend.h`)
- Purpose: implements the `imu_backend_*` contract used by `nos/imu_rec.c` for high-rate workout/diagnostic IMU streams to SD.
- Where: `imu_backend_wake` (`imu_hal.c:~70-96`) lazily creates `imu-sample` (4096 B, priority 3, PSRAM stack, `nos_mem_alloc` owner `imu.sample_stack`), `imu-write` (6144 B, priority 1, `imu.writer_stack`) and the `imu-rate` esp_timer (`:85`); `imu_backend_start(hz)` starts the periodic timer at `1e6/hz` (`:98-104`). The timer notifies the sampler, which reads via `board_imu_*` under the shared IMU mutex and pushes records into a ring; the writer drains to SD under the SD lock and mount lease. Flash/NVS are never written from these PSRAM-stack tasks.
- Cadence: runs only while a recording stream is active. Screen-off behaviour follows the stream owner (workouts keep it running).

### PCF85063 RTC and clock handling (hw-pcf85063-rtc)
- Driver: `board.c:747-846` (`board_clock_changed` write-back, `board_apply_timezone` `:830`, `board_clock_valid` `:840`) and `board_restore_clock` (`:847-888`, boot). I2C 0x51; the INT line (GPIO15) is unused, so there are no hardware alarms; the alarm app is a software compare in the main loop.
- HAL: `hal_get_datetime`/`hal_epoch_seconds`/`hal_set_datetime_checked` (`hal_esp32.c:142-174`), `hal_millis` (`:111`), TZ apply at `:822-834`.
- Calendar-day gate: `board_calendar_ready(0/1)` (`board.c:115`) blocks pedometer day rollover during clock/TZ changes (see Gaps for the bypass).

### SD card (hw-sd-card), SD lock and mount lease (task-board-fs)
- `board_sd_mount` (`board.c:558`), lease counter `board_sd_asset_acquire/release` (`:613/:621`), mutex `board_sd_asset_lock/unlock` (`:627/:628`), `board_sd_unmount` (`:629`), `board_sd_test` (`:673`). One-bit SDMMC, CLK41/CMD39/D0=40 (`pins.h`). One mutex `s_sd_lock` serialises all file I/O; unmount waits on the lease.
- 60 s CSV sensor log (`board.c:713-746`) writes the cached (low-pass) IMU snapshot, not the raw sample.
- `board_fs_task` ("board-fs", 4096 B, priority 1, created `:1042` with PSRAM caps, `board.c:919-966`): deferred SD work (settings checkpoints, CSV log, mount retries) so that no UI or ISR context blocks on the card. Storage format and checkpoint policy: [[Storage and persistence]].

### Battery ADC and power sample (hw-battery-adc)
- `hal_esp32_sample_power` (`hal_esp32.c:316-376`) is called once per second from board-sense. Reads GPIO4 (ADC1 ch3, divider 3), applies a manual calibration offset (`hal_battery_cal_set` `:311`, `hal_battery_cal_mv` `:310`), converts to percent from an OCV table with a load allowance (+25/+10) and infers USB; there is no charge-status GPIO on B. Published through `hal_power_get` (`:378`), `hal_battery_mv` (`:389`), `hal_battery_percent` (`:392`). `hal_battery_set_awake` (`:309`) switches the awake/asleep load allowance. Policy: [[Power and battery]].
- Dead on NOS: `hal_batt_log_tick` (`:1270`), `hal_batt_dump_now` (`:1287`) and the 1440-entry ring are only called from legacy `main.c`.

### BOOT button (hw-button)
- GPIO0, active low, pulled up. Configured in `hal_ext_init` (`ext_sys.c:~171`, `EXT_PIN_BUTTON`) and polled in the main loop (`nos_main.c` input handling). Also the single deep-sleep wake source: `esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0)` then `esp_deep_sleep_start()` (`nos_main.c:810-811`).

### Sleep protocol (`board_prepare_sleep`, `board_cancel_sleep`)
- `board_prepare_sleep` (`board.c:1126`) waits (5 s deadline) for board workers and SD to quiesce, flushes the RTC; `board_cancel_sleep` (`:1120`) reverses it. Called from the shutdown path in `nos_main.c:713-812`. Strap modules use the parallel `hal_ext_shutdown_begin/poll/cancel` protocol (below).

### HAL glue (`hal_esp32.c`, `core/hal.h`)
- Non-radio parts: time (`:111-174`), prefs/stores (`:203-258`; NVS namespace "wos" plus SD mirror), power (`:309-392`), backlight `hal_set_backlight` (`:437`), notification ring (`:409-416`), log ring (`:463-471`), `hal_imu_read` (`:477`), `hal_audio_bars` (`:481`, returns 0 on device), step count (`:1582`), peripheral init `hal_esp32_init_periph` (`:2312`), asset reader API (`:2500-2531`, task `asset-read`, PSRAM stack). BLE/Wi-Fi bodies belong to [[Connectivity and voice]].
- Dead on NOS: `hal_beep`, `hal_play`, `hal_mic_level`, `hal_mic_pitch`, `hal_audio_available`, `hal_set_aod` have definitions only in legacy `main.c` and the simulator.

### Strap hub (`ext/ext_sys.c`, `ext_priv.h`) - task-ext
- `hal_ext_init` (`ext_sys.c:~168-196`): configures BOOT GPIO, calls `haptic_init`, `audio_init`, `env_init`, `light_init`, `mag_init`, `hr_init`, `gps_init`, `lte_init`, then creates the `ext` task (4096 B, PSRAM stack, priority 0, any core, `:186`). Presence bits are published in `ext_present_bits`; `hal_ext_present` (`:~198`) returns them.
- `ext_task` (`:144-166`): loop calling `gps_service`, `lte_service`, `env_service`, `light_service`, `mag_service`, `hr_service`; each returns the ms until it next wants service; the task sleeps `min(50, returned) - spent` ms, so it wakes at least every 50 ms. Tracks loop gap/work max for diagnostics.
- Shutdown protocol (`ext_sys.c:~30-90`): `hal_ext_shutdown_begin/poll/cancel`; each module registers a PARK; 30 s timeout. A cancel flag is serviced by the same task (`shutdown_cancel_service`). Debug commands in `ext_debug_commands.inc`.
- Priority 0 = same priority as idle; the task can be starved by any busy priority-1+ task (see Gaps).

### Haptic (hw-strap-haptic; `ext/ext_haptic.c`) - timer-haptic
- LEDC timer 3, channel 7, 20 kHz, 10-bit PWM on IO16. Patterns are `{duty%, ms}` tables in flash; playback is an esp_timer one-shot chain (`ext_haptic.c:98-106`), so no task. Strength 1-100 % maps to 40-100 % duty. A new pattern pre-empts the playing one. Works with the screen off. Presence is always assumed (an ERM motor cannot be detected): the "present" bit for haptic is set at init.

### Audio (hw-strap-audio; `ext/ext_audio.c`, `ext_audio_files.inc`, `ext_sfx_synth.h`) - task-exta, task-extf
- MAX98357A + INMP441 on one full-duplex I2S port (IO1 BCLK, IO2 WS, IO5 amp DIN, IO3 mic SD, IO6 amp SD), 16 kHz, 32-bit stereo slots. `exta` (priority 3, PSRAM stack, `ext_audio.c:344`) pumps I2S in 4 ms blocks and is the only I2S user; `extf` (priority 0, `:350`) owns file requests so SD latency cannot stall `exta` or `ext`. I2S channels exist only while something needs them and are deleted 1.5 s after last use; amp SD is low when idle.
- Detection: `EXT_SPEAKER` is set unconditionally ("cannot be probed; default on", `:343`); `EXT_MIC` is cleared with it only on init failure (`:347`). On a bare B board with no strap, `hal_ext_present() & EXT_SPEAKER` is therefore true although no amplifier is attached.
- B has no built-in codec: the ES8311/ES7210 path of the 1.43C is not present here.

### Env sensor (hw-strap-env; `ext/ext_env.c`)
- BME280 (id 0x60) at 0x76/0x77; BMP280 (0x58) accepted with humidity 0 (`:97-110`). Forced mode every `PERIOD_MS` 2000 (`:22`), read 50 ms after the trigger; Bosch integer compensation; altitude against sea-level reference (NVS "ext_sea"). 144-deep pressure history at 10 min (`HIST_MS`, `:24`) in PSRAM, not persisted. `env_service` (`:123`) runs in the `ext` task and polls forever once detected, regardless of getter calls or screen state. Absent: `env_init` leaves the bit clear and `env_service` returns a long wait.

### Light / proximity / gesture (hw-strap-light; `ext/ext_light.c`)
- APDS-9960 at 0x39: ALS 103 ms integration, auto gain 1/4/16/64x, read every 400 ms, always on (`:16-30`, `light_service` `:172`). Lux uses `EXT_APDS_LUX_DF` 310 (uncalibrated, +-30 %, `:211`). Proximity baseline follows the floor. Gestures run only while recently requested (auto-off at 8 s idle, `:111`). Used for ambient adaptive brightness and proximity features by services (see [[Power and battery]]).

### Compass (hw-strap-mag; `ext/ext_mag.c`)
- QMC5883L (0x0D) or HMC5883L (0x1E). Active only while `hal_mag_get` is being called or calibrating, else standby (`mag_service` `:191`). Samples are discarded while the haptic motor or amp is active. Calibration (hard/soft iron, axis MAP) persisted under NVS "ext_mag" in the caller's context.

### Heart rate / SpO2 (hw-strap-hr; `ext/ext_hr.c`)
- MAX30102 at 0x57, 100 Hz FIFO drained every 120 ms by the `ext` task, shutdown when neither the user stream nor a background measurement runs. Background measurement every `BG_EVERY_MS` 10 min (<= `BG_MAX_MS` 40 s, `:34-35`, `hr_service` `:230`) appends to a 144-entry RAM history (0 = no reading). Owners are arbitrated through `hal_hr_request`. SpO2 is explicitly "NOT a medical measurement". Feeds [[Health and activity]] and [[Sports and workouts]].

### GPS (hw-strap-gps; `ext/ext_gps.c`)
- UART on IO7 (RX) / IO17 (TX). State machine `G_PROBE_START/WAKE/LISTEN/IDLE/RUN/ABSENT` (`:31`, `gps_service` `:228`): listens `LISTEN_MS` 1500 per baud at 9600, 38400, 115200 (`:28,32`) for a checksum-valid NMEA sentence, repeated for rounds, about 12.6 s total before `G_ABSENT`. When absent, `hal_gps_enable(1)` sets `enabled` and re-enters `G_PROBE_START` (`:295`). Fix data is cached and goes stale after `STALE_MS` 3000. Streams auto-stop after 8 s without getter calls.

### LTE modem (hw-strap-lte; `ext/ext_lte.c`)
- Quectel EG800AK on UART0 (IO43/IO44, 115200), PWRKEY IO18 (open-drain "press = low" default; polarity undocumented for the carrier). Non-blocking AT engine (`lte_service` `:308`): AT probe, optional PWRKEY pulse and up to 15 s wait, init script, CSQ/CEREG/CREG/COPS round-robin every 1.25 s, SMS inbox poll every 60 s and on +CMTI, send with 60 s limit, power-off via `AT+QPOWD=1`. The boot probe powers the modem once, caches SIM state and inbox, then powers it down unless enabled. SMS inbox is RAM-only. Streams auto-stop after 8 s idle.

## How it runs

| Context | Name | Prio / core / stack | Cadence |
|---|---|---|---|
| Task | `touch` (nos_main) | 5 / core 1 / `TOUCH_STACK_BYTES` | per frame awake, ~25 Hz screen off (`nos_main.c:1889`, `:588`) |
| Task | `flush` (nos_main) | 4 / core 1 / 3072 | display flush (see [[Rendering pipeline]]) |
| Task | `cmd` (nos_main) | 4 / any / 6144 | serial/BLE command line |
| Task | `board-sense` | 2 / any / 4096 PSRAM | 20 ms loop, IMU 3x per cycle, power sample 1 s (`board.c:1036`) |
| Task | `board-fs` | 1 / any / 4096 PSRAM | event-driven SD work (`board.c:1042`) |
| Task | `imu-sample` | 3 / any / 4096 PSRAM | on `imu-rate` timer notify, only while recording (`imu_hal.c`) |
| Task | `imu-write` | 1 / any / 6144 PSRAM | drains ring to SD |
| Timer | `imu-rate` | esp_timer periodic `1e6/hz` | only while recording (`imu_hal.c:85-104`) |
| Task | `ext` | 0 / any / 4096 PSRAM | wakes every <= 50 ms (`ext_sys.c:144-166`) |
| Task | `exta` | 3 / any / 4096 PSRAM | 4 ms I2S blocks while audio is needed (`ext_audio.c:344`) |
| Task | `extf` | 0 / any / 4096 PSRAM | audio file requests (`ext_audio.c:350`) |
| Timer | `haptic` | esp_timer one-shot chain | per pattern segment (`ext_haptic.c:104`) |
| Task | `asset-read` | PSRAM stack | asset requests (`hal_esp32.c:2500-2531`) |
| Timer | `pace` | esp_timer | frame pacing (`nos_main.c:1885`) |

- **Core/priority model:** FreeRTOS tick is 1000 Hz. Most driver tasks are `tskNO_AFFINITY`; the UI tasks are pinned to core 1 (`ESP_MAIN_TASK_AFFINITY_CPU0` keeps `app_main` on core 0). Stacks of board-sense, ext, exta, extf, imu tasks and asset-read live in PSRAM (`SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`), so those tasks must never write flash or NVS; NVS/flash writes (calibration, sea level) happen in the caller's context. Stack overflow checking is off (`CONFIG_COMPILER_STACK_CHECK_MODE_NONE`).
- **I2C sharing:** one master bus, one device handle per chip. `board.c` uses mutexes for touch, IMU and RTC; strap drivers call `ext_rd`/`ext_wr` helpers (`ext_sys.c:~100-140`) with `EXT_I2C_TMO_MS` timeouts and no cross-driver lock beyond the IDF bus lock. Strap I2C traffic (ENV 2 s, light 400 ms, MAG up to 50 Hz, HR FIFO 120 ms) therefore interleaves with the 150 reads/s IMU polling and touch at the transaction level.
- **Presence detection and absence:** I2C chips: `i2c_master_probe` plus ID register (`ext_sys.c:121`, `ext_env.c:101-104`). Absent modules leave their bit clear; their getters return zeros/invalid and their service functions return long waits, so cost is a single boot probe. Exceptions: GPS re-probes 12.6 s on every `hal_gps_enable(1)` while absent; LTE is probed at boot with a PWRKEY pulse and up to 15 s wait.
- **Power/DFS:** `nos_power.c` enables DFS 80..240 MHz with `light_sleep_enable=false`; there is no tickless idle, so the 20 ms IMU loop and 25 Hz touch poll keep the CPU at the DFS floor but never in light sleep.
- **Screen-off behaviour:** board-sense keeps polling the IMU and battery; touch is polled at about 25 Hz; ENV (2 s) and light (400 ms) keep running; HR runs its 10 min background measurement; GPS/LTE/MAG/audio run only while a getter or owner (workout, track) keeps them alive; haptic works. With display OFF the system can enter deep sleep (`esp_deep_sleep_start`), which stops all of this; only BOOT wakes it.
- **Shutdown/sleep:** strap: `hal_ext_shutdown_begin` asks each module to PARK (30 s timeout), `poll` reports status, `cancel` aborts. Board: `board_prepare_sleep` (5 s deadline) / `board_cancel_sleep`.

## Data in / Data out

Data in (hardware to RAM)

| Source | Raw | Cache / publisher | Rate |
|---|---|---|---|
| FT3168 | 0x38 touch packet | touch task, `board_touch_read` | 25 Hz off / frame rate on |
| QMI8658 | 14 B accel+gyro | `board.c` latest `hal_imu`, `board_imu_read` | ~150 Hz poll |
| PCF85063 | BCD time | `board_restore_clock` at boot; write on `board_clock_changed` | boot / on change |
| Battery ADC | mV on GPIO4 | `hal_power_get` | 1 Hz |
| BOOT GPIO0 | level | main loop | per loop |
| BME280 | T/P/H | `ext_env` struct + 144 history | 2 s / 10 min history |
| APDS-9960 | lux/prox/gesture | `ext_light` struct | 400 ms |
| QMC/HMC | mag XYZ | `ext_mag` struct + heading | up to 50 Hz while requested |
| MAX30102 | IR/red FIFO | `ext_hr` bpm/SpO2/PPG ring | 100 Hz, drained 120 ms |
| GPS UART | NMEA | `ext_gps` fix struct | 1 Hz-ish while enabled |
| LTE UART | AT/URC | `ext_lte` status + SMS inbox | 1.25 s round-robin |
| INMP441 | PCM | `ext_audio` mic ring/level | while requested |

Data out

| Sink | Writer | Notes |
|---|---|---|
| Haptic motor | `hal_haptic_*` to `ext_haptic` | LEDC PWM, screen-off capable |
| Amp | `ext_audio` (`exta`) | PCM/sfx on strap only |
| SD files | `board-fs`, `imu-write`, `ext_audio` file owner | under `s_sd_lock` + lease |
| NVS "wos" | `hal_store_save*` in caller context | calibrations (`ext_mag`, `ext_sea`), prefs mirror |
| RTC | `board_clock_changed` | write-back on clock/TZ set |
| Backlight | `hal_set_backlight` (`hal_esp32.c:437`) | see [[Rendering pipeline]] |
| LTE modem | `ext_lte` AT commands / SMS send | |

Consumers: [[Services hub]] (wrist-motion, sensors, sports tracks), [[Health and activity]] (pedometer via IMU, HR), [[Sports and workouts]] (GPS, HR, IMU recorder), [[Power and battery]] (battery sample, wake gating), [[Connectivity and voice]] (LTE, audio/mic), [[Settings and developer]] (device diagnostics, ext debug).

## Persistence

- No driver owns durable state of its own except: mag calibration (NVS key `ext_mag`), sea-level pressure (`ext_sea`), battery calibration (via stores), and the RTC itself (hardware, battery backed). RTC time is written on `board_clock_changed` and restored by `board_restore_clock` at boot (`board.c:847-888`).
- HR history (144), ENV pressure history (144), SMS inbox, GPS fix and battery log are RAM only; they are lost on reboot or deep sleep.
- SD is authoritative for `nosprefs` and `wifi_cred` through the checksummed alternating checkpoint (`prefs_disk`); NVS "wos" is the migration/card-absent fallback; a failed SD read blocks the NVS fallback until retried. See [[Storage and persistence]].
- The 60 s CSV log and IMU recordings go to `/sdcard` under the lease and lock.

## Interactions

- [[Engine and scheduling]]: touch and button input enter the engine through the `touch` task and main loop; driver tasks are lower priority.
- [[Rendering pipeline]]: panel, QSPI and backlight are consumed there; TE is not routed on B, so no tear sync exists.
- [[Services hub]]: services call `hal_imu_read`, `hal_gps_enable/get`, `hal_hr_*`, `hal_env_*`, `hal_light_*` each tick; the 8 s idle expiry means a service must re-call every cycle.
- [[Health and activity]], [[Sports and workouts]], [[Calorie trace]]: IMU, HR and GPS inputs; pedometer day rollover depends on `board_calendar_ready`.
- [[Power and battery]]: battery sample, DFS, deep sleep, wake sources.
- [[Storage and persistence]]: SD mount lease and lock; NVS writes only from caller contexts.
- [[Connectivity and voice]]: LTE/SMS and strap audio; B has no built-in audio.
- [[Shell and faces]], [[Utility apps]], [[Settings and developer]]: Control Center toggles, flashlight/compass/sensor apps, device diagnostics, ext debug commands.
- [[Gaps]], [[OS overview]], [[Hardware and drivers]].

## Gaps

| # | Severity | Gap | Evidence | User impact |
|---|---|---|---|---|
| 1 | high | Deep sleep wakes only on BOOT. No timer wake, no RTC INT (GPIO15), no IMU INT (GPIO8) wake. | `nos_main.c:810-811`; `pins.h:55-56` | Alarms, raise-to-wake, and scheduled work cannot wake from deep sleep. |
| 2 | medium | IMU INT and RTC INT wired but unused; IMU is polled ~150 reads/s forever. | `board.c:1021-1025`; `pins.h:55-56` | CPU and bus wake continuously; can never use light sleep; battery cost. |
| 3 | medium | Touch polled at ~25 Hz with screen off; no light sleep or tickless idle. No touch IRQ pin exists. | `nos_main.c:588`; `nos_power.c` (`light_sleep_enable=false`) | Baseline drain while screen is off. |
| 4 | medium | `EXT_SPEAKER` set unconditionally; B has no built-in audio and the strap may be absent. | `ext/ext_audio.c:343-347` | UI that gates on `hal_ext_present() & EXT_SPEAKER` offers sound that cannot play. |
| 5 | medium | Strap detection is boot-only; no hot-plug re-probe for I2C modules. | `ext/ext_sys.c:168-196` | Attaching a strap needs a reboot. |
| 6 | medium | Absent GPS re-probes (~12.6 s UART probe) every `hal_gps_enable(1)`, and workouts/tracks call it every sample/frame. | `ext/ext_gps.c:28-32,295`; `nos/svc_track.c:106`; `nos/workout.c:389,705,829,850` | Outdoor workouts without GPS spin the probe, UART churn and CPU load; possible missing-GPS UI confusion. |
| 7 | medium | Control Center LTE/GPS toggles are cosmetic against the 8 s idle expiry; `nos_prefs.lte` is never applied at boot. | `nos/hal_ext.h:27,35`; `sys_control.c:234-235`; `con_cell.c:96,194`; `sports.c:399` (sim only) | Toggle shows on but module powers down; setting not honoured after reboot. |
| 8 | medium | LTE modem powered at every boot probe with up to 15 s wait if absent; PWRKEY polarity undocumented; SMS inbox RAM only. | `ext/ext_lte.c:1-30,440-449` | Boot-time power draw, unknown wiring risk, SMS lost on reboot. |
| 9 | medium | ENV (2 s) and light sensor (400 ms) poll forever, including screen off; HR/ENV histories not persisted. | `ext_env.c:22-24`; `ext_light.c:16-30` | Extra bus traffic and battery cost; trend data lost on reboot. |
| 10 | medium | Phone/BLE TZ change bypasses `board_calendar_ready(0)` so today's steps can reset; legacy TIME-char TZ is reverted by `calendar_preferences`. | `hal_esp32.c:822-834`; `nos/svc.c:1002-1006`; `core/pedometer.c:11-14`; `nos_main.c:962` | Step count loss when phone changes timezone. |
| 11 | low | Dead battery log (1440-entry ring) in NOS build, called only from legacy main. | `hal_esp32.c:1226-1287` | Wasted RAM/code; no battery history for users. |
| 12 | low | Stale comment claims no light sensor. | `nos_main.c:658` | Misleads maintainers. |
| 13 | medium | Battery percent is an estimate (OCV table, +25/+10 load allowance, manual calibration); USB inferred, no charge-status pin. | `hal_esp32.c:292-376`; `pins.h` | Percent jumps and charge state may be wrong. |
| 14 | low | 60 s CSV log writes low-pass IMU snapshot, not raw. | `board.c:713-746` | Logs unsuitable for fine analysis. |
| 15 | low | Lux uncalibrated (DF 310, window GA). | `ext_light.c:62,211` | Auto-brightness and readouts off by about 30 %. |
| 16 | low | `hal_big_alloc` has no internal-RAM fallback; PSRAM exhaustion denies UI caches. | `nos/psram.c:49-54` | Allocation failures instead of degraded caches. |
| 17 | medium | `ext` task priority 0 can starve under busy tasks; strap I2C and touch/IMU share one bus with no cross-driver lock or priority. | `ext_sys.c:186`; `board.c:49-92` | Late strap reads, HR FIFO overruns (32-deep at 100 Hz), jittery touch under load. |
| 18 | low | No TE signal on B; no tear sync possible. | `pins.h` | Possible tearing during fast scrolls. |
