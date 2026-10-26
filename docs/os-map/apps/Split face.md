---
title: "Split face"
id: face-split
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Split face

## Summary

Every path is repo-relative. Line numbers were read from the working tree on 2026-10-23. Nothing was built, flashed or run on hardware, and no serial port was opened. Timing figures are code-derived.

- **What it is.** SPLIT is the fourth of nine home faces (`nos/faces/faces.c:15-16`, index 3). It is typographic: the hours in white and the minutes in grey, stacked on the left, with a data column on the right. The rows are HR, STP, TMP (only when weather is valid) and BAT (`nos/faces/face_split.c:1-7`, `:113`). The face record is `{ "SPLIT", enter, tick, draw, tap }` (`nos/faces/face_split.c:240`).
- **Where its data comes from.** Two service getters are called directly: `svc_heart()` and `svc_activity()` (`face_split.c:74-75`). Everything else comes from the per-frame `face_ctx` built by `build_ctx()` (`nos/apps/sys_home.c:465-489`), plus `nos_prefs` (`face_split.c:81-82`).
- **What it writes.** No service, pref, HAL or storage state. Its only private state is the static struct `S` (`face_split.c:31-39`). Its tick also updates shared face-kit state and warms the SEGMENT face's assets (see Writes).
- **Hardware.** No board-specific code in `face_split.c`. On the 1.43-B it needs the AMOLED and touch for display and taps, the QMI8658 IMU for the pedometer that feeds STP, the battery ADC on GPIO4, and the PCF85063 RTC for the clock at boot. HR needs the external MAX30102 strap. TMP needs a weather feed, which is the phone by default. The unread dot needs phone notifications.
- **Status: working.** Every display and tap path is implemented. The gaps are about data quality: a heart rate that never expires, a 50 % battery placeholder at boot, a "charging" flag that means "USB host present" on B, and weather with no staleness marker.

## Features

Shown on home page 0 when SPLIT is the selected face (`nos/apps/sys_home.c:899-902`).

- **Time.** Two-digit hour and minute. 24 h by default (`nos/prefs.c:147`). In 12 h mode the leading zero is dropped (`nos/faces/faces.c:85-90`, `nos/faces/face_split.c:63`). The tabular "1" is nudged so the left edge of the stack stays straight (`face_split.c:99-107`).
- **Digit roll.** On a minute change only the changed digits turn, clipped to their cell, in held steps on a two-frame cadence (`face_split.c:53-73`, `:198-208`).
- **Build-in and wake.** On show, the digits turn in and the data rows fade in over four perceptual ink steps (`face_split.c:42-50`, `:198-213`). On wake from AOD the grey outlines fill in (`face_split.c:173-184`). A gap of more than 500 ms off the glass settles the digits without a roll (`face_split.c:64`; `nos/faces/faces.c:457-461`).
- **HR row.** Last BPM, or "--" when there is none (`face_split.c:125-128`). The value turns red at 120 BPM or above, or below 40 (`face_split.c:109`, `:128`).
- **STP row.** Step count, or "N.NK" at 10,000 and above (`face_split.c:132-133`). The step goal is not shown.
- **TMP row.** Temperature rounded to whole degrees, in C or F from `units_metric` (`face_split.c:80-82`, `:136-137`). Drawn only when `weather_valid` is set (`face_split.c:113`).
- **BAT row.** Percent, or "--" with no reading. A bolt precedes the value when charging (`face_split.c:140-141`, `:159-162`). Label and value turn red at 15 % or below when not charging (`face_split.c:110`, `:142`).
- **Unread dot.** An 8 px dot at (233,24) that fades in and out over four ink steps. It shows when any notification is unread (`nos/faces/faces.c:552-580`; `face_split.c:214`). The count is not shown.
- **AOD.** Time as grey outlines, plus one grey BAT value when the percent is known. HR, STP and TMP are hidden (`face_split.c:185-195`).
- **Taps.** HR opens Heart rate, STP opens Activity, TMP opens Weather (`face_split.c:218-238`). Holding lights the row (`nos/apps/sys_home.c:1062-1066`; `nos/faces/faces.c:830-835`).

Not on the face: date, weekday, seconds, step goal or progress, next alarm, weather condition, high and low, notification count, battery pill.

## Reads

### Direct calls (`nos/faces/face_split.c`)

| Call | Used for | Line | Source |
|---|---|---|---|
| `svc_heart()` then `->bpm_last` | HR value and HR alarm | `:74`, `:76` | Getter `nos/svc.c:581`. Written by `heart_tick()` (`nos/svc.c:586-617`, set at `:603-604`) and `svc_heart_record_at()` (`nos/svc.c:625-643`). |
| `svc_activity()` then `->steps` | STP value | `:75`, `:77` | Getter `nos/svc.c:438`. Written by `act_publish()` (`nos/svc.c:440-455`) from `hal_activity_get()` (`firmware/main/board.c:495-507`). |
| `nos_prefs.units_metric` | TMP unit and conversion | `:81-82` | `nos/prefs.h:28`; default 1 at `nos/prefs.c:156`. |
| `nos_prefs.hour24` | 12 h or 24 h digits | via `fk_hh()` (`nos/faces/faces.c:79-90`) | Default 1 at `nos/prefs.c:147`. |

`S.bpm` (`:76`) and `S.steps` (`:77`) fall back to values the context already holds. `c->bpm` is `svc_heart()->bpm_last` (`nos/apps/sys_home.c:473`), and `c->steps` already falls back to `steps_today` (`sys_home.c:471`). The fallbacks never change the result.

### Face context (`nos/faces/face.h:14-27`, filled in `nos/apps/sys_home.c:465-489`)

| Field | Set from | Line | Underlying source |
|---|---|---|---|
| `c->dt` | `hal_get_datetime()` | `sys_home.c:468` | `firmware/main/hal_esp32.c:142-146`, which uses `gettimeofday` and `localtime_r` (`:116-128`). The clock is restored from the PCF85063 at boot (`firmware/main/board.c:847-881`). |
| `c->battery` | `svc_sys()->battery_pct`, else `hal_battery_percent()` | `sys_home.c:474` | `sys_tick()` in `nos/svc.c:987-996`, once a second. `hal_power_get()` (`firmware/main/hal_esp32.c:378-387`). Sampler at `:318-362`. Boot default 50 at `:287`. |
| `c->charging` | `svc_sys()->charging` | `sys_home.c:475` | `external_power == 1` (`nos/svc.c:992`). |
| `c->bpm` | `svc_heart()->bpm_last` | `sys_home.c:473` | As above. |
| `c->steps` | `svc_activity()->steps` | `sys_home.c:471` | As above. |
| `c->unread` | `svc_notif_unread()` | `sys_home.c:476` | Count of unread entries in the notification store (`nos/svc.c:238-242`). The face only tests `> 0`. |
| `c->weather_valid`, `c->temp_c` | `svc_weather()` | `sys_home.c:478-479` | `wx_from_hal()` (`nos/svc.c:389-410`, from `hal_weather_get()` at `firmware/main/weather_service.c:57-70`), or the phone's `gb_weather()` (`nos/svc.c:1073-1100`). |
| `c->t`, `c->aod`, `c->enter`, `c->rec` | Home state | `sys_home.c:483-488` | `nos_t`, the AOD flag, the build-in ramp, and the REC chip ramp. |

### HAL and helper calls reached through face-kit code

- `hal_millis()` in `fk_wake_tick()` (`nos/faces/faces.c:460`), called from `face_split.c:57`.
- `fk_red()` reads `c->rec` (`nos/faces/face_kit.h:151-155`).

### Not read

No direct call to `svc_weather()`, `svc_sys()`, `svc_notif_*`, `svc_alarm_next()`, `hal_epoch_seconds()`, `hal_ext_present()`, `hal_hr_get()`, or any NVS or SD key. These context fields are also unused: `c->step_goal` (`sys_home.c:472`), `c->next_alarm_min` (`sys_home.c:481-482`), `c->wx_cond` (`sys_home.c:480`) and `c->lite`. A grep of `face_split.c` finds none of `fk_date`, `fk_status`, `next_alarm`, `step_goal`, `wx_cond`, `lite`, `fk_ghost`, `hal_ext`, `hal_epoch`, `svc_notif` or `svc_weather`.

## Writes

No persistent write, and no `svc_*` mutator, `prefs_*` call or `hal_*` request is made from the face. Mutations are:

- **Own state `S`** (`face_split.c:31-39`): digit strings `from` and `to`, roll start times `t0[4]`, `init`, cached values, the `fk_wake w` and `red`. Reset by `enter()` (`:41`) and on a stale tick (`:64-68`).
- **Wake state** (`face_split.c:57`; `nos/faces/faces.c:458-477`): the face's `fk_wake`.
- **Shared unread fade** (`face_split.c:58`; `nos/faces/faces.c:548`, `:552-559`): `s_unread_k`, shared with other faces.
- **Shared AOD and second clock** (`face_split.c:56`; `nos/faces/faces.c:34`, `:40`, `:57-64`): `fk_aod_dx`, `fk_aod_dy`, `s_sk` and `s_dc_epoch`.
- **SEGMENT warm-up.** `fk_aod_update()` calls `face_segment_warm()` (`nos/faces/faces.c:61`) when the face is not in AOD and `enter >= 1`. That loads SEGMENT's font, sprites and track, about 1 ms a frame until done (`nos/faces/face_kit.h:171-172`). SPLIT's tick does this work for another face.
- **Framebuffer.** Digits, data rows and the unread dot (`face_split.c:166-215`).
- **Simulator probes.** The `fk_probe*` calls at `face_split.c:83-96` compile only under `GFX_COST_MODEL`, so they are not in the watch build.

## Background behaviour

The face's own code runs only while home draws it. The services behind it run whether or not the face is on glass.

| Runner | Where | Period and priority | When |
|---|---|---|---|
| Split `tick` and `draw` | Home frame loop: `nos/apps/sys_home.c:625` (tick), `:901` (draw) | Nominal NOS_FPS 40 (`nos/render_config.h:8`). Presentation target 60 (`:13`). | Screen on, face is the current face, not hidden by apps (`sys_home.c:619-625`). In AOD the same tick runs at 1 Hz (`firmware/main/nos_main.c:2114-2116`). Not in PWR_OFF. |
| `svc_tick()` | `nos/nos.c:2341`, via `services_tick()` (`nos/apps/registry.c:62-65`) | Frame rate when the screen is on. 1 Hz in AOD and PWR_OFF (`firmware/main/nos_main.c:2094`, `:2114`). | Always. Inside: `sys_tick`, `act_tick` and `sports_tick` each second (`nos/svc.c:1315-1320`). `heart_tick` every 60 s (`:1322`). `wx_tick` every 10 s (`:412-414`). |
| `board-sense` task | `firmware/main/board.c:967`, created at `:1036` | Priority 2, tskNO_AFFINITY, 4 KB stack. Loop about 20 ms (`board.c:1021-1024`). | Always, unless parked for sleep. Calls `pedometer_update()` (`board.c:989`), publishes `s_activity` (`:1009`), and samples power each second (`:1021`). |
| `ext` task | `firmware/main/ext/ext_sys.c:186` | Priority 0, unpinned. | Always. Runs the HR driver, which takes a background reading every 10 min, up to 40 s (`firmware/main/ext/ext_hr.c:34-35`). |
| Weather fetch task | `firmware/main/weather_service.c:175` | Priority 2, 8 KB stack. | Only when Wi-Fi is enabled (`firmware/main/hal_esp32.c:2391-2394`). Off by default (`nos/prefs.c:150`). |
| Phone weather | `nos/svc.c:1174`, into `gb_weather()` (`:1073`) | Event-driven (Gadgetbridge message). | Whenever the phone sends weather. |
| Pedometer checkpoint | `firmware/main/board.c:903-955` | Next save 60 s after a success, retry after 5 s. | Writes NVS key `pedometer`. |
| Activity checkpoint | `nos/svc.c:558` | Every 10 min when dirty, retry after 5 s. | Writes NVS key `nosact`. |

Screen off (PWR_OFF): the Split face does not tick. In AOD it ticks once a second, with its AOD draw. Its inputs keep updating in both states. `svc_tick()` still runs at 1 Hz (`firmware/main/nos_main.c:2094`), so the battery, steps and heart values refresh on that cadence. The heart value changes at most once a minute from the background path (`nos/svc.c:1322`), or immediately when a live reading is recorded.

On wake, `sys_home.c:547` requests a build-in. `fk_build()` returns 1 while the wake morph runs (`nos/faces/faces.c:478-480`), so the face skips the build-in and does the AOD-to-awake fill (`face_split.c:198`).

## Persistence

- **The face stores nothing.** No NVS, SD or prefs writes. `S` and the face-kit globals are RAM, reset on boot or on `enter()`.
- **Preferences** (`nos_prefs`, `nos/prefs.h:7-46`): `units_metric`, `hour24` and the face index `face` (SPLIT = 3). `prefs_save()` debounces for 2 s, with a 10 s maximum dirty age (`nos/prefs.h:55`). On a board with SD the checkpoint goes to `/sdcard/watch/settings.{0,1}.bin` with an NVS mirror (`firmware/main/hal_esp32.c:228-256`; `nos/prefs_disk.c:32`). Card-absent boots fall back to NVS.
- **Face choice** is written by the picker when it closes (`nos/apps/sys_faces.c:374-375`) and by the serial `face` command (`firmware/main/nos_main.c:1389`).
- **Activity** (`nosact`) goes to NVS namespace `wos` on both boards. `hal_store_save_checked()` sends only `nosprefs` and `wifi_cred` to SD (`firmware/main/hal_esp32.c:236`). The load path is at `:270-282`.
- **Pedometer snapshot** goes to NVS key `pedometer` (`firmware/main/board.c:915`; load at `:1069`).
- **Heart rate** is RAM only. `s_heart` is static (`nos/svc.c:72`). The 144-sample history is a PSRAM ring in the ext driver (`firmware/main/ext/ext_hr.c:384-392`). Both are lost on reboot.
- **Weather** is RAM only. `s_wx` is static (`nos/svc.c:69`). The phone resends after a reboot.
- **Clock.** The PCF85063 holds the time on boards with a valid RTC (`firmware/main/board.c:847-881`). Without one, the clock stays at epoch 0 until USB, BLE or SNTP sets it (`firmware/main/board.c:883-886`).

## Navigation

**Opened from**
- Home pager, page 0, when `nos_prefs.face == 3`. Index 3 is SPLIT in `nos/faces/faces.c:15-16`. Drawn at `nos/apps/sys_home.c:899-902`.
- Face picker. Choosing SPLIT sets `nos_prefs.face` and saves (`nos/apps/sys_faces.c:366-377`). A long press on the face opens the picker (`nos/apps/sys_home.c:1135-1140`).
- Serial `face` command. Sets the index and restarts the build-in (`firmware/main/nos_main.c:1389`).
- Home wake and face change call `sys_home_build_in()` (`nos/apps/sys_home.c:437`, `:547`). Boot does too (`nos/apps/sys_boot.c:108`).

**Opens**
- Row 0, HR: `scr_heart` (`nos/apps/fit_heart.c:938`), returned by `face_split_hit()` (`face_split.c:225`).
- Row 1, STP: `scr_activity` (`nos/apps/fit_activity.c:547`), via `face_split.c:225`.
- Row 2, TMP, only with four rows: `scr_weather` (`nos/apps/out_weather.c:907`), via `face_split.c:226`.
- BAT row and the time: no target. `face_split_hit()` returns 0 (`face_split.c:225-226`).

**Dispatch.** An `IN_TAP` on page 0 calls the face's `tap()`, then `nos_open()` (`nos/apps/sys_home.c:1126-1127`). A REC chip hit is checked first (`sys_home.c:1119-1124`). The press highlight comes from `nos_face_press_rect()` (`nos/faces/faces.c:830-835`) on touch-down while held (`sys_home.c:1062-1066`).

**Hit test** (`face_split.c:218-236`). Accepts x at or beyond 276. Each row has a 50 px band centred on `row_y - 6`. The press rectangle covers only label to value end (`face_split.c:227-231`).

Related notes: [[Heart rate]], [[Activity]], [[Weather]], [[Home pager]], [[Face picker]], [[Face registry and shared face kit]], [[Shell and faces]].

## Gaps

1. **[medium] Heart rate never expires.** `bpm_last` is replaced by any newer good reading and is never reset (`nos/svc.c:603-604`, `:631-632`). The face shows it with no age (`face_split.c:76`, `:127`), and it stays red if it was out of range (`face_split.c:109`, `:128`). A reading taken before the strap came off stays on the face as the current heart rate. `bpm_last_epoch` (`nos/svc.h:107`) exists but is not read here.

2. **[medium] 50 % battery placeholder.** The HAL starts with `battery_percent=50` (`firmware/main/hal_esp32.c:287`) and keeps it until a stable reading (`firmware/main/hal_esp32.c:350-361`). The face takes `battery_pct` unchecked (`nos/apps/sys_home.c:474`; `face_split.c:78`, `:140`). `battery_valid` (`nos/svc.h:180`) is never read. A fresh boot shows BAT 50, including in AOD (`face_split.c:186-191`). When the sample is older than 3 s, `hal_power_get()` keeps the last percent and sets charging to 0 (`firmware/main/hal_esp32.c:381-386`). That also shows with no marker.

3. **[medium] On B, "charging" means a USB host is present.** B has no charge-status input. The only `PIN_CHG_STAT` read is compiled under `#ifdef` (`firmware/main/hal_esp32.c:324-326`). `usb` comes from `usb_serial_jtag_is_connected()` (`firmware/main/hal_esp32.c:323`). That sets `external_power=1` (`:337`, `:350-351`), which feeds `charging`. The face then draws the bolt (`face_split.c:159-162`) and drops the low-battery red (`face_split.c:110`). A watch at 10 % connected to a PC shows a bolt and no alarm. This is from source reading only and has not been checked on a device.

4. **[low] Weather temperature has no staleness marker.** While the phone feed is under 3 h old, `valid` stays 1, and `stale` is set at 1 h (`nos/svc.c:412-421`). After 3 h the service falls back to HAL weather (`nos/svc.c:423`). The pager copies only `valid`, `temp_c` and `cond` (`nos/apps/sys_home.c:477-480`). The face draws whenever `weather_valid` is set (`face_split.c:80-82`, `:113`). A temperature up to 3 h old is shown as current.

5. **[low] No "clock not set" state.** When the RTC is invalid and nothing has set the clock, system time is set to epoch 0 (`firmware/main/board.c:883-886`). The face shows the local time derived from that. It never checks `hal_epoch_seconds()` (`firmware/main/hal_esp32.c:148-153`) or the board clock-valid flag. An unsynced watch shows a wrong time with no hint.

6. **[low] The HR row gives no reason for "--".** Without the strap the HR row shows "--" beside its label (`face_split.c:127`). The face does not check `hal_ext_present()`, so "no strap" looks the same as "no reading yet". B has no HR pins (`firmware/main/pins.h`). The Heart rate screen shows "NO HR SENSOR" in its widget.

7. **[low] No hardware fallback for STP on boards without an IMU.** The pedometer runs only from IMU samples (`firmware/main/board.c:989`). The 1.43C has no IMU (`CLAUDE.md`, hardware table), so on that board STP has no sensor source. This is from source reading and the repo record, not a device check. The face shows whatever the activity service holds.

8. **[low] Red is used outside alerts.** The header says red appears "only as an alert" (`face_split.c:6-7`). The unread dot uses red whenever no alarm is showing (`face_split.c:214`; `nos/faces/faces.c:565-579`). The comment and the code disagree. This is a documentation gap, not a functional fault.

9. **[low] Tap zone is wider than the press highlight.** `face_split_hit()` has no right-hand bound (`face_split.c:220`), so a tap anywhere right of x=276 on a row opens its screen. The press highlight covers label to value end only (`face_split.c:227-231`). The tap works where nothing lights.

10. **[low] The data column jumps when TMP appears or drops.** Row centres depend on `nrows()` (`face_split.c:113-114`): HR sits at y=165 with four rows and y=190 with three. Rows are drawn at those positions with no tween (`face_split.c:155`, `:210-213`). The whole column moves 25 px at once when weather validity changes.

11. **[low] No date.** `fk_date()` exists (`nos/faces/faces.c:107-110`), but SPLIT does not call it. The face has no weekday or date.

12. **[low] Next alarm is built but not shown.** `c->next_alarm_min` is filled from `svc_alarm_next()` (`nos/apps/sys_home.c:481-482`) and never read by SPLIT.

13. **[low] No step goal.** `c->step_goal` is built (`nos/apps/sys_home.c:472`) and never read by SPLIT. Steps show a bare count with no goal or progress.

14. **[low] Notifications show a dot, not a count.** `c->unread` is a count (`nos/apps/sys_home.c:476`; `nos/svc.c:238-242`). The face only tests `> 0` (`nos/faces/faces.c:554`).

15. **[low] The battery row cannot be tapped.** `face_split_hit()` maps rows 0 to 2 only (`face_split.c:225-226`). The battery settings screen is not reachable from the face.

16. **[low] The face has no options of its own.** Rows, their order and their content are fixed. The face reads no per-face preference. Only `units_metric` and `hour24` affect it.

17. **[low] Comments disagree with the 40 Hz frame rate.** `nos/faces/face_kit.h:30-31` says the build-in is "0.40 s = 12 frames". `nos/faces/faces.c:455` says `WAKE_N` 6 is "0.2 s at 30 fps". NOS_FPS is 40 (`nos/render_config.h:8`), and the code counts whole frames at M_FPS (`nos/faces/faces.c:70-77`). At 40 Hz the wake morph is 0.15 s. The build-in is still driven by the 0.40 s `ENTER_S` clock (`nos/apps/sys_home.c:55`), so the "12 frames" wording is what is wrong.

## See also

- [[Services hub]]: where `svc_heart`, `svc_activity`, `svc_sys` and `svc_weather` are fed.
- [[Health and activity]]: the HR and step data this face shows.
- [[Sports and workouts]]: the sensor and pedometer journals.
- [[Power and battery]]: the battery percent, the USB-as-charging rule, and the 50 % boot value.
- [[Storage and persistence]]: the prefs checkpoint and the NVS keys `nosact` and `pedometer`.
- [[Hardware and drivers]]: the IMU, RTC, battery ADC and MAX30102 strap.
- [[Connectivity and voice]]: phone weather and notifications, and the Wi-Fi weather path.
- [[Shell and faces]]: the home pager, the face table and the tap dispatch.
- [[Engine and scheduling]]: the frame loop, NOS_FPS and the 1 Hz AOD and screen-off cadence.
