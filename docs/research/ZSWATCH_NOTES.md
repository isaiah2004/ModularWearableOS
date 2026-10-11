# ZSWatch research notes

Retrieved 2026-10-11 from `ZSWatch/ZSWatch` `main` (via the GitHub API tree and
raw.githubusercontent.com) and from zswatch.dev. Paths below are relative to
https://github.com/ZSWatch/ZSWatch/blob/main/. ZSWatch runs on an nRF5340
(512 KB RAM) with a 240x240 round touch LCD and LVGL. Licence: GPL-3.0, so
borrow **ideas, protocols and numbers, not code**.

Key sources:
- Architecture: https://zswatch.dev/docs/development/architecture (`website/docs/development/architecture.md`)
- Writing apps: https://zswatch.dev/docs/development/writing_apps
- Phone setup: https://zswatch.dev/docs/getting-started/phone_setup
- Gadgetbridge parser: `app/src/ble/gadgetbridge/ble_gadgetbridge.c`
- Bangle.js protocol reference: https://www.espruino.com/Gadgetbridge

---

## 1. Applications (`app/src/applications/`)

Every app registers itself with `SYS_INIT` and has a picker category (Root/Tools/Fitness/System/Games/Sensors/Random).

| App (name shown) | Category | What it does | Needs |
|---|---|---|---|
| Watchface (not in picker) | - | Home screen. Pluggable faces (`ui/watchfaces/*`: digital, minimal, pixel, analog "dial" ports of Felix Biego's ESP32 faces). Shows time, battery, steps, weather, notification count, pressure, music, BLE status. Pull-down control panel. | RTC, IMU steps, pressure, BLE |
| Face | System | Watchface picker with preview images | - |
| Notification (hidden) | System | Notification list. Opened by swiping left on the watchface | BLE |
| Music | Root | Track/artist/progress plus play/pause/next/prev. Sends commands back over BLE. Built with LVGL Editor XML | BLE (GB or AMS) |
| Weather | Root | Current conditions plus multi-day forecast. Asks the phone for GPS (`gps_power`), then fetches open-meteo **through the phone's HTTP proxy** | BLE + Bangle GB build |
| Compass | Root | Heading from sensor fusion, with a timed "rotate 360 deg around x, y, z" calibration popup | Magnetometer + IMU |
| Fitness | Root | Today's steps plus a 7-day bar chart. Hourly samples stored in flash (`zsw_history`), daily reset at 23:59 | BMI270 step counter |
| Timers | Root | Multiple countdown timers/alarms backed by `zsw_alarm` (RTC) | RTC |
| Lap Time | Root | Stopwatch with laps, on the 100 ms periodic tick | - |
| Update | Root | FOTA over BLE (MCUmgr/SMP) or USB. Enables XIP/MCUmgr only while active | BLE/USB |
| Memos | Root | Voice memos: Opus-encoded to LittleFS. The phone downloads, transcribes, classifies and returns a result | Mic (I2S) |
| Settings | Root | Lists: Display (brightness, always-on, animations, smooth seconds), Bluetooth (pairable, delete bonds, BLE log), General (vibrate on click, swipe-up-back, reset steps, reboot, factory reset), Developer (erase ext flash, AoA/CTE) | - |
| Battery | Tools | Voltage/% graph over a week (15-minute samples persisted), plus PMIC info (TTE/TTF, charge state) | Fuel gauge/PMIC |
| Calc | Tools | Calculator driven by a Zephyr SMF state machine | - |
| Flashlight (hidden) | Tools | Full-white screen at 100% brightness. Opened from the pull-down | - |
| Ppt_remote | Tools | BLE HID "next/prev slide" presentation clicker | BLE HID |
| DA7212 Test | Tools | Audio codec/speaker test | Extension board |
| Mic | (dev) | Live FFT spectrum analyzer | Mic |
| Sensor | Sensors | Live summary: pressure, temperature, light | BMP581, APDS9306 |
| Fusion | Sensors | 3D orientation (quaternion/Euler) at 100 ms | IMU (+mag) |
| Debug | System | Uptime, time-to-sleep, retained-RAM stats (display-on time, resets), coredump summary | - |
| About | System | Firmware version, app count, filesystem stats | - |
| Trivia | Games | True/false quiz from opentdb.com through the GB HTTP proxy | BLE + Bangle GB |
| 2048 | Games | Game | - |
| QR | Random | Shows a QR code (your link) at max brightness | - |
| X-ray | Random | Gag "x-ray of your wrist" image at 100% brightness, brightness restored on exit | - |
| ZDS App | Random | Zephyr Dev Summit demo. Rotating logo, wrist-shake gesture resets it | IMU gesture |
| Chronos | Random | UI for the alternative "Chronos" Android companion protocol (`ble/chronos`) | BLE |

Heart rate (MAX32664 hub) exists only on the WIP extension board (`sensors/zsw_health_data.c`).

---

## 2. OS architecture

**Layers** (architecture doc): sensors and BLE publish to **zbus channels**. Managers orchestrate. Apps subscribe and render with LVGL. Init runs on the system workqueue so the large init stack is not held forever (`app/src/main.c`).

**App manager** (`managers/zsw_app_manager.{c,h}`):
- `application_t { start_func(root, group), stop_func, back_func -> bool consumed, ui_unavailable_func, ui_available_func, name, icon, hidden, category, current_state }`.
- States: `STOPPED -> UI_VISIBLE <-> UI_HIDDEN -> STOPPED`. `UI_HIDDEN` means the app keeps running with the screen off, and apps must not touch the UI (on ZSWatch the UI code lives in external flash, which is unmapped).
- Max 25 apps. Start/close are deferred through one-shot `lv_timer`s (50 ms) so they never run inside an input callback.
- `zsw_app_manager_show(close_cb, root, group, app_name)`: with a name it launches straight into that app ("launch-only"), and back then returns to the watchface instead of the picker. Shortcuts use this (swipe left opens "Notification", tap on battery opens "Battery").
- Back: call `back_func()` first. If the app doesn't consume it, stop the app and redraw the picker, or close an open folder first.
- Listens to `activity_state_data_chan` and moves the running app between VISIBLE and HIDDEN when the screen turns on or off.

**zbus channels** (`app/src/events/*`):

| Channel | Payload |
|---|---|
| `accel_data_chan` | `zsw_imu_evt_t {type: XYZ/DOUBLE_TAP/STEP/STEP_ACTIVITY/TILT/GESTURE/SIG_MOTION/WRIST_WAKEUP/NO_MOTION/ANY_MOTION; union{xyz, step.count, step_activity(still/walk/run), gesture(push_arm_down, pivot_up, wrist_shake, flick_in, flick_out)}}` |
| `battery_sample_data_chan` | `{mV, percent, temperature, avg_current, tte, ttf, status, error, vbus_current_limit_ma, is_charging, pmic_data_valid}` |
| `ble_comm_data_chan` | `{type: NOTIFY, NOTIFY_REMOVE, SET_TIME, WEATHER, MUSIC_INFO, MUSIC_STATE, REMOTE_CONTROL, HTTP, GPS; union of typed structs}` (`ble/ble_comm.h`) |
| `activity_state_data_chan` | `{ACTIVE, INACTIVE, NOT_WORN_STATIONARY}` |
| `music_control_data_chan` | `{CLOSE, PLAY, PAUSE, NEXT, PREV}`, sent from the app to BLE |
| `zsw_notification_mgr_chan` / `_remove_chan` | new notification / removed `{notification}` |
| `light_data_chan` `{lux}`, `magnetometer_data_chan` `{x,y,z}`, `pressure_data_chan` `{pressure, temperature}`, `environment_data_chan` (legacy) | sensor samples |
| `periodic_event_100ms_chan`, `_1s_chan`, `_10s_chan` | empty tick |
| `voice_memo_*_chan` | recording state/result |

Rule: listeners run **in the publisher's thread**, so they copy data and `k_work_submit()`. They never touch LVGL directly.

**Periodic events** (`events/zsw_periodic_event.c`): three shared ticks (100 ms / 1 s / 10 s). They are **lazy**: the timer starts when the first observer is added (`zsw_periodic_chan_add_obs`) and is cancelled when the last is removed. Light and pressure sensors poll on 10 s, the magnetometer on 1 s, the stopwatch on 100 ms, timers on 1 s. Battery sampling runs on its own work item. Data path: `tick -> sensor read -> zbus pub -> app listener -> k_work -> UI update if VISIBLE`.

**Managers** (`app/src/managers/`):
- **Power manager**: activity state machine, idle timer, tilt-away detection, display power. Turns vibration off below 3750 mV. Details in section 5.
- **Notification manager**: fixed array of **10** notifications, each `{id, timestamp, sender[50], title[50], body[50], src enum}`. When full it evicts the oldest (lowest id). It maps `src` strings ("Messenger", "WhatsApp", "Gmail", "Home Assistant", "Discord", "LinkedIn", "Reddit", "YouTube", calendar...) to icon enums, so a popup shows an app icon.
- **Phone app publisher**: on each battery sample sends `{"t":"status","bat":..,"volt":..,"chg":..}` and an `act` packet with steps. On notification removal it sends `DISMISS` to the phone, so dismissal syncs.
- Others: SMP (MCUmgr file access/FOTA), XIP (QSPI flash on/off), USB, microphone, speaker, recording.
- `zsw_history`: generic ring buffer persisted to the settings store (`init(max_samples, sample_size, buf, key)`, `add`, `get`, `save`, `load`). Used for battery (a week at 15 min) and steps (a week hourly).
- `zsw_retained_ram_storage`: counters that survive resets (display-on time, display-off time, reboot count).

**Sensor fusion** (`sensor_fusion/zsw_sensor_fusion.c`): xioTechnologies **Fusion** AHRS at 100 Hz on a delayable work item, gain 0.5, NWU convention, gyro offset auto-calibration (`FusionOffset`). It runs `FusionAhrsUpdate` with the magnetometer when one is present, else `UpdateNoMagnetometer`. The heading comes from `FusionCompassCalculateHeading`. It is **only started by the Compass/Fusion apps** (`init` in start, `deinit` in stop).

---

## 3. UI and navigation (`ui/zsw_ui_controller.c`)

- **States**: `INIT (onboarding) -> WATCHFACE <-> APP_MANAGER`. Onboarding runs once (settings flag).
- **Watchface gestures**: swipe **left** opens the Notification list, swipe **right** opens the Face picker, swipe **up** opens the app picker, swipe **down** opens the **pull-down panel** (`ui/watchfaces/zsw_watchface_dropdown_ui.c`). The panel holds a scrolling now-playing label, Music, Flashlight, Shutdown and Settings round buttons, a brightness slider (saved on release) and battery % with charge state/TTE.
- **Watchface tap targets** raise events (`WATCHFACE_APP_EVT_CLICK_BATT/STEP/MUSIC/SETTINGS/FLASHLIGHT`) that deep-link into apps.
- **App picker** (`ui/app_picker/app_picker_ui.c`): **circular layout of 7 slots (6 on a ring and 1 in the centre) per page**, with left/right paging. Categories appear as coloured **folder** items. A folder opens an overlay grid with a title and a close button. The picker remembers the last page and folder.
- **Back**: a dedicated button (KEY_3), or optional **swipe-up-to-go-back** (setting, off by default). One `handle_back_action()` closes, in priority order: recording overlay, voice popup, notification popup, then the app.
- **Buttons vs touch**: buttons come in through Zephyr's input subsystem. On the watchface they are OS shortcuts (KEY_1 opens the picker). Inside apps they feed an LVGL **encoder** indev (left/enter/right) with focus groups (`is_buttons_for_lvgl`). A separate `temp_group` captures input while a notification popup is up.
- **Wake handling**: `zsw_power_manager_reset_idle_timout()` returns true if the press woke the screen, and **the waking press is swallowed**. Raw touch coordinates don't wake the screen. Only processed events (gestures/keys) do.
- **Notification popup** (`ui/notification/zsw_popup_notification.c`): on a new notification, `main.c` vibrates (`PATTERN_NOTIFICATION`) and shows a panel with app icon, sender and body, plus an X button. Auto-close after 10 s. Tapping the panel expands it and extends the timeout to 20 s. Any swipe dismisses it. Closing removes it from the manager, which sends DISMISS to the phone. If one is already showing, the new one is queued (`pending_not_open`).
- **Generic popup** (`ui/popup/zsw_popup_window.c`): `zsw_popup_show(title, body, close_cb, seconds, show_buttons)`. Used for calibration, warnings and pairing.
- **Vibration patterns** (`drivers/zsw_vibration_motor.h`): CLICK, NOTIFICATION, FIND (looping).

---

## 4. Phone integration

### Gadgetbridge (Android): Bangle.js protocol over Nordic UART Service
- Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`. RX (phone writes) `...0002`, TX (watch notifies) `...0003` (`ble/ble_transport.h`). The NUS UUID goes in the scan response along with the name. ZSWatch also advertises DIS and solicits ANCS.
- Pairing: in Gadgetbridge enable "Discover unsupported devices", long-press the found device and choose **Bangle.js**. Use the **"Bangle.js Gadgetbridge"** Play Store build if you want HTTP (internet) access.
- **Phone to watch**: lines of the form `\x10GB({...})\n` (JSON with quoted keys; packets span several MTU chunks). ZSWatch counts `{`/`}` from `GB(` until they balance (`ble_gadgetbridge_input`). Time arrives **separately**, not inside GB(), as JavaScript, e.g. `\x10setTime(1700556601);E.setTimeZone(1.0);(s=>{...})(...)\n`. Parse the integer in `setTime(` and the float in `E.setTimeZone(` (hours).
- Messages (Espruino reference, with the fields ZSWatch actually uses in **bold**):
  - `{"t":"notify","id":**int**,"src":**"WhatsApp"**,"title":**".."**,"subject":"..","body":**".."**,"sender":"..","tel":"..","reply":bool}`. ZSWatch shows title as the sender.
  - `{"t":"notify~","id":..,"body":..}` (modified, ZSWatch ignores it) and `{"t":"notify-","id":**int**}` (remove).
  - `{"t":"weather","temp":**K**,"hi":K,"lo":K,"hum":**%**,"rain":%,"uv":..,"code":**OWM code**,"txt":**".."**,"wind":**..**,"wdir":**deg**,"loc":".."}`. **Temperatures are Kelvin**.
  - `{"t":"musicinfo","artist","album","track","dur":s,"c":count,"n":num}` (all used).
  - `{"t":"musicstate","state":"play|pause","position":s,"shuffle":..,"repeat":..}` (all used).
  - `{"t":"call","cmd":"incoming|outgoing|accept|reject|start|end","name":..,"number":..}`. **ZSWatch does not implement this.** Implement it for a full-screen call UI.
  - `{"t":"find","n":bool}`: ZSWatch loops a vibration and keeps the screen awake.
  - Also defined, not used by ZSWatch: `alarm {d:[{h,m,rep(weekday bitmask),on}]}`, `vibrate {n}`, `act {hrm,stp,int}` (realtime request), `calendar`/`calendar-`, `nav {instr,distance,action,eta}`, `gps {...}`, `http {resp|err,id}`.
  - Non-ASCII quirk: Gadgetbridge may send Latin-1 bytes raw (`0xF6`) or as `\xe4` escapes. ZSWatch checks UTF-8 validity and otherwise converts Latin-1 to UTF-8 (`convert_to_encoded_text`). Copy this behaviour.
- **Watch to phone**: plain JSON lines ending in `\n`:
  - `{"t":"status","bat":0-100,"volt":mV,"chg":0|1}`. ZSWatch sends mV, while the reference says volts as a float.
  - `{"t":"ver","fw":"..","hw":".."}` about 3 s after connect.
  - `{"t":"music","n":"play|pause|next|previous|volumeup|volumedown"}`. **Note:** ZSWatch's sprintf leaves the value unquoted (`"n": play`). Quote it.
  - `{"t":"notify","id":..,"n":"DISMISS|DISMISS_ALL|OPEN|MUTE|REPLY","msg":".."}`.
  - `{"t":"call","n":"ACCEPT|END|REJECT|IGNORE|..."}`, `{"t":"findPhone","n":true}`.
  - `{"t":"act","stp":steps,"hrm":bpm,"act":"WALKING|RUNNING|NOT_WORN|ACTIVITY","rt":1}`.
  - `{"t":"http","url":"https://..","id":"7"}` (HTTPS only, Bangle build only). The reply is `{"t":"http","id":"7","resp":"<escaped string>"}` or `{"err":".."}`. ZSWatch un-escapes `\"` and matches the id, with a timeout.
  - `{"t":"gps_power","status":true}` makes the phone stream `gps` packets. Weather uses this to get lat/lon.
  - `{"t":"info|warn|error","msg":".."}` shows a toast on the phone.
- BLE tuning (`ble/ble_comm.c`): exchange MTU on connect (send chunk = MTU-3). Use a short connection interval for 5 s after connect, then **400-500 ms**. Advertise slow (1-1.2 s) by default and fast (100-150 ms) while pairable.
- ZSWatch adds its own types (`watchface_bg`, `voice_memo`, `smp`, `log`, `reset`) for its companion app. Gadgetbridge ignores unknown types, so a private extension can share the same pipe.

### iOS
Native **ANCS** (title, subtitle, message, app id, date, positive/negative action labels, app display name), **AMS** (track artist/album/title/duration, playback info, remote commands) and **CTS** time (`ble/ble_ancs.c`, `ble_ams.c`, `ble_cts.c`). These publish the same `ble_comm_data_chan` types, so the UI doesn't care which phone OS is connected. Pairing currently needs nRF Connect to bond. On an ESP32-S3 ANCS is doable with NimBLE as a GATT client.

---

## 5. Power management (`managers/zsw_power_manager.c`, `drivers/zsw_display_control.c`)

- States: **ACTIVE** (screen on, CPU at 128 MHz for render and SPI speed). **INACTIVE** (display sleep/blank, touch suspended, CPU slow, QSPI XIP off). **NOT_WORN_STATIONARY** (display regulator fully off).
- Idle timeout is 20 s by default (Kconfig `POWER_MANAGEMENT_IDLE_TIMEOUT_SECONDS`), measured from **LVGL's inactive time**. A "display always on" setting disables it. The watch stays active for at least 1 s after waking.
- **Tilt-away sleep** (a good idea): while active, sample accel every 100 ms. Learn a "facing" reference by averaging 4 normalised gravity samples, skipping any with |a| outside 5-15 m/s². Later, when there has been no touch for 1.5 s and dot(current, ref) <= 0.45 for 800 ms, go inactive. dot >= 0.75 counts as facing. Any user activity re-learns the reference.
- **Wake**: BMI270 wrist-wake (wear-wake-up) interrupt, button, or a processed touch gesture. The **flick-out** gesture puts the watch to sleep immediately.
- **Two-stage off**: on INACTIVE, enable the IMU **NO_MOTION** interrupt. When it fires, cut display power and arm **ANY_MOTION**. Motion re-powers the display (still asleep) and returns to INACTIVE, so the next wake is fast.
- Rendering: the LVGL handler is a delayable work item that is cancelled while the display sleeps. On wake the whole screen is invalidated, and the backlight only comes on after the first frame renders (no stale-frame flash). Brightness fades via timer/counter steps.
- Lazy periodic timers and VISIBLE/HIDDEN app states mean nothing redraws with the screen off.
- Measured: keeping QSPI XIP alive cost 3-5 mA. Turning it off when the screen is off brought idle from about 5 mA to about **500 uA** (blog: https://zswatch.dev/blog/progress-hw-xip-usb-power).
- Below 3750 mV, vibration is disabled.

---

## 6. Sensors

- Current hardware (README): **BMI270** IMU (step counter, step-activity classifier, wrist gestures, wrist-wake, any/no-motion, all on-chip and interrupt-driven), **BMP581** pressure (about 20 cm altitude resolution), **LIS2MDL** magnetometer, **APDS-9306** light sensor, **RV-8263** RTC, nPM1300 PMIC/fuel gauge, PDM/I2S mic. Earlier revisions used a Bosch BME688 environment sensor. The `environment_data_chan` row in the docs is a leftover, and no BME688 code remains in the tree. The light sensor is polled every 10 s and published, but **nothing in the tree uses it for auto-brightness yet**.
- **Steps**: entirely the BMI270's on-chip counter. The firmware keeps a software offset (`zsw_imu_set_step_offset`) and resets at 23:59. The Fitness app stores hourly samples for 7 days, and daily totals are the max of each day's samples.
- **Compass calibration**: hard-iron only. During a timed window (Kconfig seconds) the user rotates the watch around all axes. The magnetometer tracks min/max per axis, offset = (max+min)/2, saved to the settings store (`"calibr"` key) and subtracted from every reading. The soft-iron matrix in Fusion is identity. The heading is tilt-compensated via accelerometer + mag (`FusionCompassCalculateHeading`).

---

## 7. Ten ideas to borrow (ranked by value for this ESP32-S3 / 466x466 AMOLED / one-button watch)

1. **Speak Gadgetbridge's Bangle.js protocol over NUS.** It gives Android notifications, music, weather, find-device and time sync with no companion app to write. Implement `notify`, `notify-`, `musicinfo/musicstate`, `weather` (Kelvin), `call`, `find`, `setTime`+`E.setTimeZone`. Send `ver` and `status` on connect and `notify n:DISMISS` on dismissal. Include the Latin-1 to UTF-8 fix. Add the `http` proxy for weather/trivia-style apps (Bangle build).
2. **App lifecycle with VISIBLE/HIDDEN states and `back_func` that can consume.** The screen-off state goes to each app, so timers keep running while drawing stops. This maps directly onto your damage/frame engine: a HIDDEN app produces no damage.
3. **Lazy shared tick channels (100 ms / 1 s / 10 s)** that only run while something subscribes. This is cheaper than per-app timers and makes "nothing redraws with the screen off" true by construction.
4. **Typed pub/sub bus with work-queue hand-off** (a tiny zbus equivalent: a FreeRTOS queue or esp_event plus a per-type struct union). BLE/sensor tasks publish, and the UI task consumes. Never render from the BLE callback.
5. **Tilt-away auto-sleep plus wake gesture**, using ZSWatch's numbers (learned reference, 0.75/0.45 dot hysteresis, 800 ms hold, 1.5 s touch guard). Your QMI8658 has no wrist-wake engine, so do wake in software on the same gravity-vector math (raise plus face-up), and use its any/no-motion interrupt for the two-stage display-off.
6. **Swallow the waking input.** The press or touch that turns the screen on must not also activate something. A one-line rule with a big UX payoff.
7. **Notification store of 10 entries with src-to-icon mapping, a popup with timeout (10 s, extend on tap), and dismissal synced back to the phone.** On a one-button watch: the button dismisses the popup, a tap expands it, and a swipe dismisses it.
8. **Watchface as a vtable of setters** (`set_time`, `set_battery`, `set_steps`, `set_weather`, `set_music`, `set_num_notifications`, `get_preview_img`). The controller pushes data, so faces stay dumb and swappable, and a face picker gets previews for free. Fits your sprite/damage design: each setter dirties only its own rect.
9. **Deep-link launch (`launch_app("Battery")`) with "launch-only" back semantics,** plus a pull-down quick panel (brightness slider saved on release, flashlight, music, settings, battery). On a one-button watch, back from a deep-linked app returns to the face, not the launcher.
10. **Persisted ring-buffer history and retained counters** (battery every 15 min, steps hourly for 7 days, display-on time and reset counts in RTC memory). These are cheap, and they give you a battery graph and a "why did my battery drain" debug screen. On ESP32-S3, use NVS or a LittleFS file plus `RTC_NOINIT_ATTR`.

Honourable mentions: generic `popup_show(title, body, seconds)`, the hard-iron min/max compass calibration (only if you add a magnetometer), MCUmgr-style BLE FOTA, ANCS/AMS for iPhone, and the "backlight on only after the first frame renders" rule (with an AMOLED: hold panel-off or brightness 0 until the first full frame is out, to avoid a stale-frame flash).
