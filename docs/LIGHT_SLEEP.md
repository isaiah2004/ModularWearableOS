# Automatic light sleep (1.43-B)

Status: implemented and compile/host tested on 2026-10-23. **Not yet run on hardware**; the
checklist at the end lists what must be verified on the watch. The 1.43C keeps its previous
no-sleep power policy (`nos_power.c`: `sleep_wanted=0` under `CONFIG_WATCH_BOARD_143C`).

## Policy

| Display | Light sleep | CPU | Wakes |
|---|---|---|---|
| ON | never (`nos-display` lock) | DFS 80..240 MHz, unchanged | frame pacing, unchanged |
| AOD entry animation | never | as ON | as ON |
| AOD / SENSOR / OFF | allowed whenever every lock is free | sleeps between wakes | shared 50 ms grid |

Light sleep is IDF automatic light sleep: `esp_pm_configure(240, 80, light_sleep_enable=true)` with
`CONFIG_FREERTOS_USE_TICKLESS_IDLE`. The idle task sleeps when every task is blocked for at least
`FREERTOS_IDLE_TIME_BEFORE_SLEEP` (8 ticks). Any held PM lock blocks it. Locks:

- `nos-ui` (CPU_FREQ_MAX): main loop active scope, unchanged. Released before every non-ON wait.
- `nos-prewarm` (CPU_FREQ_MAX): dot prewarm task, unchanged.
- `nos-display` (NO_LIGHT_SLEEP): held while display ON and during the AOD entry lead
  (`nos_main.c`, `go_state` plus a per-iteration refinement). Needed because the UI task drops
  `nos-ui` while it waits for the next frame.
- `nos-xfer` (NO_LIGHT_SLEEP): held by the command task during BLE/SD transfers (`ble_xfer.c`).

Failure handling: if the sleep configuration is refused, DFS stays on and sleep reports off; if
anything else fails, `nos_power` falls back to fixed 240 MHz exactly as before. Mode changes
(`nos_power_set_max_mhz`) keep `light_sleep_enable` set so IDF's ~8.6 KB CPU-retention buffer is
allocated once, not freed and re-allocated per mode change.

## Wake grid

Every periodic task that runs while the display is not ON rounds its next wake to a multiple of
50 ms of the FreeRTOS tick clock (`firmware/main/sleep_grid.h`; the tick clock is kept in step with
wall time across light sleep). One CPU wake then serves all of them. Rounding starts from
`now + period - 10 ms`, so a task that wakes up to 10 ms late still lands on the next grid point
instead of the one after. Wake rate while idle: at most 20 per second.

Tasks on the grid: main loop (50 ms, was 20 ms), touch poll (50 ms, was 40 ms), IMU batch reader
(50 ms), USB command read (50 ms), 1 Hz workers (weather, filesystem worker, Wi-Fi/BLE housekeeping),
the external-sensor task. Event driven instead of polling: workout SD writer (notified per record),
filesystem worker (notified by retry/RTC requests, 1 s idle), `ble_xfer_poll` (1 s callout).

## IMU

Display ON: legacy 448 Hz sampling, three register reads per 20 ms, unchanged.

Display not ON: the QMI8658 streams accel+gyro at 112.1 Hz (ODR code 6, period 8921 us) into its
128-sample FIFO (stream mode) and the board task drains it every ~50 ms (about 6 samples; the FIFO
holds 1.1 s). Register set: CTRL2=0x26, CTRL3=0x56, CTRL5=0x70, FIFO_CTRL=0x0e. Reads use CTRL9
`REQ_FIFO` (0x05) and `RST_FIFO` (0x04) with the STATUSINT CmdDone handshake. Samples are timestamped
backwards from the drain time at the nominal period (`board_fifo_stamp`, resynchronised after an
overflow). Consumers are unchanged: knock needs the ~112 Hz stream and millisecond stamps, the
step detector gets every second sample (it was tuned at ~50 Hz), motion wake is dt based and
resets only above a 200 ms gap, and `hal_imu_sample` validity is 150 ms.

Fallbacks: three failed (re)configurations or three silent streams give a strike each; after three
strikes the task degrades to one register read per 50 ms until reboot. In that mode knock detection
is degraded (20 Hz is too slow) and the state is visible in `BOARD_IMU_BATCH mode=2 degraded=1`.
The raw IMU recorder keeps its own register-read pacing. Going ON restores the legacy register set.

## Button wake

GPIO0 is the only GPIO wake source. `gpio_wakeup_enable` overwrites the pad interrupt type with
LOW_LEVEL, so it is armed per sleep by the enter hook and undone by the exit hook, which restores
ANYEDGE and, because the wake consumed the press edge, records the press for the main loop. The
enter hook also calls `gpio_sleep_sel_dis` for the panel (QSPI CS/CLK/D0-D3, RST, EN) and SD
(CLK/CMD/D0) pins: `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND` / `CONFIG_PM_SLP_DISABLE_GPIO` isolate
every other pad in automatic sleep, which would drop panel enable and lose the AOD frame. The
deep-sleep path (`shutdown`) removes the hooks and the GPIO wake source before its ext0 wake.

## USB

`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`: IDF holds NO_LIGHT_SLEEP while a USB host is connected, so
the watch never sleeps with the serial console in use. `hal_external_power()` is unchanged.
Plugging in while asleep is only noticed after the next grid wake (<= 50 ms to ~1 s depending on the
task that observes it); this needs a hardware check.

## Radios

Wi-Fi: unchanged (MIN_MODEM default; `WIFI_PS_NONE` is never used). BLE: modem sleep enabled
(`BT_CTRL_MODEM_SLEEP_MODE_1`) with the main XTAL as the low-power clock and kept powered in light
sleep (`BT_CTRL_LPCLK_SEL_MAIN_XTAL`, `BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP`), because the
internal 150 kHz RC (`RTC_CLK_SRC_INT_RC`) is too inaccurate to keep a BLE connection across sleeps.
The BLE control callout runs at 1 s (100 ms ON). The 1.43C shares `sdkconfig.defaults`, so it also
gets BLE modem sleep and tickless idle, but never enters light sleep.

## Accounting

- `NOSPM ... light_sleep=%d display_held=%d xfer_held=%d sleep_ms=%u sleep_entries=%u` (serial
  `NOSPM` report): sleeps shorter than 500 us (IDF rejected the sleep) are not counted.
- Minute log (board.c, SD CSV): trailing columns `sleep_ms,wakeups` hold per-minute deltas (header
  `...,cpu_mhz,sleep_ms,wakeups`). Old files keep their old header; readers must tolerate the
  extra columns.
- `BOARD_IMU_BATCH mode degraded strikes batches samples overflows errors` in the `board` report.

Expected idle behaviour: `sleep_ms` close to 60000 minus a few ms per wake, and `wakeups` of roughly
1200 per minute at the 20 Hz grid. A low `sleep_ms` means a lock is held or a task is not on the grid.

## Sdkconfig (firmware/sdkconfig.defaults, appended block)

`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_PM_LIGHT_SLEEP_CALLBACKS=y`,
`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`, `CONFIG_BT_CTRL_MODEM_SLEEP=y`,
`CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y`, `CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y`,
`CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y`, `CONFIG_PM_LIGHTSLEEP_RTC_OSC_CAL_INTERVAL=64`.
Already set and relied on: `CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_HZ=1000`,
`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y` (derived), `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP`
unset (peripherals keep their state), `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y`.

## Tests

- `tests/nos_power_test.c` (all modes in `tools/test_optimizations.ps1`): lock gating, display and
  transfer locks, retention-flag stability, sleep counters, failure fallbacks.
- `tests/sleep_grid_test.c`: grid alignment, lateness tolerance, wrap.
- `tests/board_protocol_test.c`: FIFO sample count/overflow decode and timestamp spacing.
- `tests/test_board_regressions.py`, `tests/test_sd_asset_leases.py`: harness stubs updated for the
  batch path and the new log columns.

## Hardware verification checklist

1. Button wake latency from OFF and from AOD, including a very short press and a long hold.
2. Knock-to-lock and knock-to-unlock while the display is off (needs the FIFO stream; check
   `BOARD_IMU_BATCH` reports `mode=1 degraded=0`).
3. BLE connection (phone bridge) survives minutes of sleep; notification delivery latency.
4. Step count keeps increasing while the display is off and the watch is carried.
5. AOD panel paints once per second and holds the frame between paints (GPIO isolation check).
6. `NOSPM` and minute-log `sleep_ms` / `wakeups` match the expectation above; average current drops.
7. Plug USB in while asleep: console and charging indication appear, no stuck sleep, unplug resumes sleep.
8. Wi-Fi reconnect and weather refresh still work with the display off.
