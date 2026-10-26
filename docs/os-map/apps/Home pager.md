---
title: "Home pager"
id: sys-home
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Home pager

Source: `nos/apps/sys_home.c` (1184 lines). Read-only source review of the 1.43-B build. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Scope is the root screen `scr_home` and the calls it makes. The hosted pages (Activity, Heart, Weather, Music, Timer) and the embedded Apps list are owned by other notes and appear here only where the pager calls them.

## Summary

The Home pager is the NOS root screen. It is `scr_home` (`nos/apps/sys_home.c:1176-1177`), set as the root by `nos_apps_boot()` (`nos/apps/registry.c:73`) after the boot screen (`nos/apps/nos_apps.h:8`).

It is a horizontal pager of seven pages (`nos/apps/sys_home.c:48-51`):

- page 0: the current watch face, `nos_faces[nos_prefs.face]`
- page 1: the embedded Apps list (`sys_launcher_page_*`)
- pages 2-6: five glance widgets, `wg_activity`, `wg_heart`, `wg_weather`, `wg_music`, `wg_timer` (`:203`)

Physical order left to right is Activity, Heart, Weather, Music, Timer, Face, Apps (`:3-6`, slot mapping at `:287-291`).

The pager owns no data. Each home frame it snapshots the services into a `face_ctx` (`build_ctx`, `:465-490`), draws the page under the finger, and routes touch and button input. It writes nothing persistent. Its only durable effect is the face choice, which the face picker writes through `prefs_save()`.

Hardware the pager's own code needs: the panel and touch (it draws and routes input), the IMU and RTC only through the services it reads, PSRAM for the page blur cache (`nos/pblur.c:41-55`), and the battery ADC. The haptic tick needs the strap motor on IO16 (see Gaps).

Status: working. The pager, page physics, REC chip, AOD entry ramp and input routing are complete in this file. The weak points are data freshness and the USB-based charging flag on B (see Gaps).

## Features

- **Face page (page 0).** Draws the selected face with `f->draw(&sub, &H.c)` (`nos/apps/sys_home.c:899-902`). There are nine faces in `nos/faces/faces.c:15-16`: DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE, ORRERY. A face build-in runs on first show and on wake (`:547`, `:433-437`), lasting `ENTER_S = 0.40 s` (`:55`, `:278-284`, `:485`). The serial face command also restarts it (`firmware/main/nos_main.c:1389`). A tap on the face is offered to the face's `tap()` hook, which may open a screen (`:1126-1127`). The face also receives touch first while it fills the glass (`:1026-1028`).
- **Apps page (page 1).** The embedded launcher list. It is entered and left by the pager's lifecycle (`:445`, `:458`, `:604-609`) and drawn at `:903`.
- **Five glance widgets (pages 2-6).** Each widget gets `tick(dt, visible)` every frame, `shown()` on its first visible frame, and `draw`, `tap` and `press` hooks (`nos/apps/nos_apps.h:61-69`; `:626-636`). The widgets are fixed and unordered (`:203`).
- **Pager physics.** The content follows the finger 1:1. It rubber-bands past both ends (`:1096-1104`). On release the finger's velocity picks the page (`FLICK_V = 350 px/s`, `:54`, `:365-382`). A critically damped pixel spring finishes the motion and cannot overshoot (`:352-363`). Programmatic moves (side button, air gestures) are mechanical cuts (`:386-394`).
- **Return to face as one page.** From a widget page, the side button draws the current widget in slot -1 for the trip, so the return is one page of motion (`:396-411`, `alias` at `:218`).
- **Page dots.** Seven dots at `UI_DOTS_Y` (`nos/ui.h:63`). They fade in over 4 frames and out over 9 after a 6-frame hold (`:587-595`, `:664-695`). They are not touch targets (see Gaps).
- **Haptic detent.** Every page crossing calls `nos_feedback(FB_TICK)` (`:585`), which fires `hal_haptic(HAPTIC_TICK)` when `nos_prefs.haptics` is on (`nos/nos.c:570`).
- **REC chip.** Shown on the face line and in the control centre while a track, voice memo, IMU recording or sports workout runs (`:143-181`). Priority is memo, then IMU, then workout (SPORT or PAUSE), then GPS track (`:150-173`). Labels are `REC mm:ss`, `IMU mm:ss`, `SPORT mm:ss` or `PAUSE mm:ss`. Tapping it opens the matching app after 3 lit frames: `scr_memo` (kind 2), `scr_imu` (3), `scr_workout` (4), `scr_nav` (1) (`:174`, `:516-521`). The chip's place on each face is in `k_rec_slot` (`:188-194`). The face's own red yields to it through `ctx.rec` (`:486-488`).
- **Unread dot.** On widget pages a single red dot at `(UI_CX, 24)`, radius 8, fades in with the slide (`:986-992`). On the face, the face draws its own dot from `ctx.unread` (`nos/faces/faces.c:522`, `:554-568`).
- **Pressed states.** A complication under the finger lights its press rect after 60 ms of stillness (`:308-349`, `:1055-1071`). A widget page gets its `press(x, y, down)` hook (`:329`, `:345`).
- **Face picker.** A long press on the face opens `scr_faces_home` in place (`:1135-1144`). ORRERY can open it through `home_open_faces()` after its own hold (`:1008-1014`). The charging rim is drawn during the hold (`:993-1003`).
- **Control centre and notifications.** From the face only, a vertical drag down opens `scr_control` (TX_DROP) and a drag up opens `scr_notifs` (TX_RISE) (`:1087-1093`).
- **Double press.** The second press of a double press opens the last app opened from the launcher (`:1157-1162`, see Navigation).
- **Air gestures.** Left and right gestures step one page, with `FB_LIMIT` at the ends (`:1163-1170`).
- **AOD.** The face only. The widget pages and the pager are not drawn. The face dims out over 3 frames, the AOD face comes up over 3 more, and the 1 Hz AOD frames continue after that (`:245-269`, `:538-546`, `:945-957`). The pager is reset to the face on AOD entry (`:543`).

## Reads

Services called directly from `sys_home.c`. Every one is a getter. The file calls no `svc_*` mutator.

| Call | Where in sys_home.c | Source of the data |
|---|---|---|
| `svc_activity()` (`steps`, `goal`) | `:469-472` | Getter `nos/svc.c:438`. `act_publish()` (`svc.c:440-456`) takes the goal from `nos_prefs.step_goal_100` (`svc.c:442`). `act_tick()` (`svc.c:529-566`) reads `hal_activity_get()` (`firmware/main/board.c:495-505`), which is the pedometer snapshot (`board.c:1009`). Runs at 1 Hz (`svc.c:1315-1319`). |
| `svc_sys()` (`battery_pct`, `charging`, `steps_today`) | `:470-475` | Getter `svc.c:986`. `sys_tick()` (`svc.c:987-996`) copies `hal_power_get()` (`firmware/main/hal_esp32.c:378-388`) once a second. `steps_today` is set in `act_publish` (`svc.c:454`). |
| `svc_heart()` (`bpm_last`) | `:473` | `heart_tick()` (`svc.c:586-623`) runs every 60 s (`svc.c:1322`). `svc_heart_record_at()` (`svc.c:625-632`) is called by `nos/apps/fit_heart.c:554,663` and `nos/apps/fit_breathe.c:353`. |
| `svc_notif_unread()` | `:476` | Counts unread flags in the PSRAM notification store (`nos/svc.c:238-242`). |
| `svc_weather()` (`valid`, `temp_c`, `cond`) | `:477-480` | Getter `nos/svc.c:337`. `wx_tick()` (`svc.c:412-424`) runs every 10 s (`svc.c:1323`). It uses the phone feed when fresh (under 3 h), else `hal_weather_get()` (`firmware/main/weather_service.c:57`). |
| `svc_alarm_next()` (keeps `in_min` only) | `:482` | `nos/svc.c:768-800`. The alarm list is loaded from NVS key `nosalarm` (`svc.c:732-736`). |
| `svc_memo_active()`, `svc_memo_elapsed_ms()` | `:152-153` | `nos/apps/con_memo.c:509-510`: `s_rec_open` and `hal_rec_ms()`. |
| `svc_track_active()`, `svc_track_label()` | `:171` | `nos/svc_track.c:312`, `:332-337`. |
| `workout_get()` (state, `elapsed_ms`) | `:163-170` | `nos/workout.h:127` (service and UI thread only). |

HAL and clock reads:

- `hal_imu_record_get()` (`:158`): a locked copy of the IMU recorder status (`nos/imu_rec.c:163-164`). Used for state and elapsed time only.
- `hal_get_datetime()` (`:468`): the system clock (`firmware/main/hal_esp32.c:142-146`). The RTC restores it at boot (`firmware/main/board.c:871`). Without a valid RTC it stays unset until USB, BLE or SNTP sets it (`board.c:885`).
- `hal_battery_percent()` (`:474`): reached only when `battery_pct` is 0. It reads the same `hal_power_get()` struct, so the fallback adds nothing (`firmware/main/hal_esp32.c:392`).
- `hal_millis()` (`:436`, `:448`, `:528`): `firmware/main/hal_esp32.c:111`.

Preferences (RAM struct `nos_prefs`, read-only in this file):

- `nos_prefs.face` (`:273-274`, `:435`, `:552`). Defined in `nos/prefs.h:20`.
- `nos_prefs.step_goal_100` (`:472`). Fallback goal only, because `svc_activity().goal` is normally set.
- `nos_prefs.sleep_s` (`:531`). Used only to detect a wake from sleep.

Indirect preference reads: `nos_feedback()` reads `nos_prefs.haptics` and `nos_prefs.sound` on every tick, tap and open (`nos/nos.c:570-572`).

Function tables the pager calls: `nos_faces[]` and `nos_face_count` (`nos/faces/faces.c:15-17`), the face hooks `enter`, `tick`, `draw`, `tap` (`nos/faces/face.h:29-36`), the face extensions (`nos/faces/face_ext.h:16-25`), the widget hooks (`nos/apps/nos_apps.h:61-69`), and `sys_launcher_last()`, which returns `nos_last_app()` (`nos/apps/sys_launcher.c:139`).

Engine state (not data): `nos_t`, `nos_frame`, `nos_top()`, `nos_transitioning()`, `nos_touch_travel()`, `nos_aod`, `nos_subview()`, the motion-blur calls, and `nos_face_style()`, `nos_face_input()`, `nos_face_interactive()`, `nos_face_press_rect()`.

## Writes

Nothing is persisted by this file. There are no `svc_*` mutators, no `prefs_*` calls, no `nos_prefs` assignments, no NVS or SD writes.

RAM only:

- The pager state struct `H` (`nos/apps/sys_home.c:205-244`), including the face context `H.c` (`:223`), the pressed-state struct `PR` (`:313-319`), `s_open_next` (`:426`), `s_bake_face` and `s_bake_page` (`:907-908`), `s_rec_wy` (`:779`) and the static `next` in `home_blur_tick` (`:932`).

PSRAM blur cache (`nos/pblur.c`, allocated with `hal_big_alloc`, `:55`):

- `pblur_invalidate(0)` on a face change (`sys_home.c:552`), `pblur_check` (`:927`) and `pblur_tick` (`:938`) bake page snapshots during idle frames.

Prewarm budgets (in RAM): `sys_launcher_prewarm`, `sys_notifs_prewarm`, `set_settings_prewarm` and `sys_control_prewarm` (`:644-647`, `:934`).

Engine calls that change the screen stack or the user's device:

- `nos_open()` for the REC chip (`:519`), the deferred open (`:525`), a face tap (`:1127`), a double press (`:1160`), and widget taps through their own `tap` hooks (`:1129`).
- `nos_push(&scr_faces_home)` (`:1012`, `:1140`).
- `nos_pull(&scr_control, TX_DROP)` and `nos_pull(&scr_notifs, TX_RISE)` (`:1090-1091`).
- `nos_feedback()` (`:585`, `:1154-1155`, `:1166`). These drive the motor through `hal_haptic()` and the sound through `hal_sfx()`, gated by prefs.
- Apps page lifecycle, `sys_launcher_page_enter`, `_leave`, `_tick`, `_input`, `_cancel` (`:398`, `:415`, `:445`, `:458`, `:539`, `:604-609`, `:1019-1044`).

Indirect writes through hosted widgets, not by this file: the Timer quick-start starts a service timer through `tm_start` (`nos/apps/time_timer.c:1187`). The Music disc tap calls `player_toggle()`, which calls `svc_music_cmd()` (`nos/apps/con_music.c:124`, `:955-957`).

The face picker writes `nos_prefs.face` (`nos/apps/sys_faces.c:374`) and calls `prefs_save()` (`:375`). A serial debug command also sets it (`firmware/main/nos_main.c:1389`).

## Background behaviour

The pager is not a task. Its tick is the top screen's tick in the UI loop.

- **Loop and cadence.** `app_main` (`firmware/main/nos_main.c:1899`) runs the frame loop on the main task, CPU0 (`firmware/sdkconfig:1707`). Its priority is not overridden in `firmware/sdkconfig`, so it is the IDF default. Frames are paced at `NOS_PRESENT_FPS = 60` (`nos/render_config.h:13`), with authored motion at `NOS_FPS = 40` (`:8`). `frame_run()` calls `nos_bg_tick` (`nos/nos.c:2341`) and then `tick_screens()` (`:2347`).
- **Only the top screen ticks.** `tick_screens()` (defined at `nos/nos.c:2101-2110`) ticks only the top screen unless a transition is running. While another screen covers home, `home_leave()` sets `H.covered` (`nos/apps/sys_home.c:455-457`) and home does not tick.
- **Per-frame work while home is on the glass.** `build_ctx()` runs every frame and calls `svc_alarm_next()` and `svc_notif_unread()` (`:465-490`), even though no face reads the alarm (see Gaps). All five widget `tick()` hooks run every frame, visible or not (`:626-636`). The heart widget refreshes its store every 60 s even while hidden (`nos/apps/fit_heart.c:979-990`). Idle prewarm and blur baking are budgeted (`:638-649`).
- **Services.** `services_tick()` calls `svc_tick()` and `background_tick()` (`nos/apps/registry.c:62-66`). `svc_tick()` (`nos/svc.c:1303-1328`) runs track and memo ticks every frame (`:1308-1309`), and at 1 Hz runs `sys_tick`, `act_tick` and `sports_tick` (`:1315-1319`). It runs `workout_tick` every call (`:1321`), heart every 60 s (`:1322`), weather every 10 s (`:1323`), and timers, alarms, calls and ringer every call (`:1325-1328`).
- **Screen off.** The loop branch for `PWR_OFF` (`firmware/main/nos_main.c:2091-2100`) does not tick screens, so home does not tick. It calls `nos_bg_tick(1.0f)` once a second (`:2094`), so `svc_tick` runs at 1 Hz with `dt = 1.0`, which covers the track, memo and workout ticks (`nos/svc.c:1308-1321`). The loop then sleeps 20 ms.
- **AOD and sensor-idle.** The `PWR_AOD` and `PWR_SENSOR` branch (`firmware/main/nos_main.c:2104-2122`) runs `nos_frame_run()` once a second (`:2114-2118`), so home ticks at 1 Hz and draws the face only (`nos/apps/sys_home.c:945-957`). Frames run at target rate only while the AOD entry ramp is running (`sys_home_aod_entering`, `:266-269`).
- **Sleep decision.** Sleep fires after `sleep_s` of no input when the top screen lacks `SCR_KEEPAWAKE` and no finger is down (`firmware/main/nos_main.c:2078-2084`, `nos/power_policy.h:18-19`). `nos_awake_ms()` is reset by `nos_keep_awake()` (`nos/nos.c:584-586`). Home's flags are `SCR_NOBACK | SCR_HDRAG | SCR_NATIVE_CACHE` (`nos/apps/sys_home.c:1176-1177`), so home does not keep the device awake. The REC chip does not either (see Gaps).
- **Other tasks on the board.** The touch task runs at priority 5 on core 1 (`firmware/main/nos_main.c:1889`). The flush task runs at priority 4 on core 1 (`:1887`). The command task runs at priority 4 unpinned (`:1891`). The dot-bake task runs at priority 1 on core 1 (`:1950`). The board sense task runs at priority 2 (`firmware/main/board.c:1036`). The services hub and the pager both run in the UI loop, not in these tasks (`nos/svc.h:3-5`).

## Persistence

- **The pager persists nothing.** The state `H` lives in RAM. A cold boot starts on the face because `home_enter()` zeroes `H` on first entry (`nos/apps/sys_home.c:430-438`). The page position is not saved. AOD entry forces the face (`:541-544`).
- **Face choice.** `nos_prefs.face` is written by the picker and saved with `prefs_save()` (`nos/apps/sys_faces.c:374-375`). `prefs_save()` debounces for 2 s and sets a 10 s maximum age (`nos/prefs.c:260-265`, `nos/prefs.h:55`). The write is `hal_store_save_checked("nosprefs")` (`nos/prefs.c:349`). On B, that routes to the checked SD files `/sdcard/watch/settings.{0,1}.bin` (`firmware/main/hal_esp32.c:243`, `nos/prefs_disk.c:32`) and mirrors to NVS. A failed save stays dirty and is retried (`nos/prefs.h:57-58`). Only `nosprefs` and `wifi_cred` go to SD. All other keys go to NVS (`firmware/main/hal_esp32.c:236`).
- **Data the pager shows.** Alarms are NVS key `nosalarm` (`nos/svc.c:728`, `:735`). Activity is NVS key `nosact` (`svc.c:499`, `:565`). Notifications, weather, the heart reading and the battery values are RAM only (`svc.c:8` lists only `nosalarm` and `nosact` as persisted).
- **REC chip.** Its state is RAM. The recorders that drive it own their own files. Those are documented in their notes.

## Navigation

Entered from:

- The boot screen replaces itself with home (`nos/apps/nos_apps.h:8`).
- A pushed screen returns to home on a side-button press at stack depth above 1 (`nos/nos.c:951-952`, `nos_home()`). Home itself has no swipe-back (`SCR_NOBACK`, `nos/apps/sys_home.c:1176-1177`).

Opens (screen to target):

- Apps page (page 1): a horizontal slide from the face, or the side button on the face (`:1151-1156`, `go_page(1)` at `:1154`).
- Control centre: drag down on the face (`:1087-1093`, `scr_control`).
- Notifications: drag up on the face (`:1087-1093`, `scr_notifs`). Neither sheet can be pulled from a widget page or the Apps page.
- Face picker: long press on the face (`:1135-1144`, `scr_faces_home`) or ORRERY's hold (`:1008-1014`).
- Activity widget: `scr_activity` (`nos/apps/fit_activity.c:581`).
- Heart widget: `scr_heart` (`nos/apps/fit_heart.c:1023`).
- Weather widget: `scr_weather` (`nos/apps/out_weather.c:938-943`).
- Music widget: `scr_music` through `nos_open(&scr_music, UI_CX, WG_CY)` (`nos/apps/con_music.c:953-967`). The centre disc toggles play and pause instead (`:953-958`).
- Timer widget: `scr_timer`, `scr_stopwatch` or `scr_alarm` by its mode, and quick-start circles start a timer at once (`nos/apps/time_timer.c:1178-1196`).
- REC chip: `scr_memo`, `scr_imu`, `scr_workout` or `scr_nav` by kind (`nos/apps/sys_home.c:174`, `:519-520`).
- Double press: the last app opened from the launcher, `sys_launcher_last()` (`:1157-1162`). It returns `nos_last_app()`, which is set only for apps in the launcher list (`nos/apps/registry.c:45-52`). Every REC target is in that list, so the chip path works.
- Long press on the side button, when home does not consume it: the power sheet (`nos/nos.c:953-955`).

Air gestures step pages (`:1163-1170`). Face tap targets open whatever the face returns from `tap()` (`:1126-1127`).

Opened from: the boot screen (`nos/apps/nos_apps.h:8`), and any pushed screen through the side button (`nos/nos.c:951-952`). `sys_home_open_after()` (`nos/apps/sys_home.c:428`, `:522-526`) reopens a queued app once home is back on top.

## Gaps

Ordered by severity. Items are from source reading. Items marked "verify on device" are code evidence without a bench run.

1. **[medium] Battery shows a boot placeholder and does not check validity.** The default `battery_percent` is 50 (`firmware/main/hal_esp32.c:287`), and the code comment at `:352-353` calls it a placeholder the UI showed. The percentage changes only on a USB-range voltage or after 10 stable reads (`hal_esp32.c:350-361`). `svc_sys_t` carries `battery_valid` (`nos/svc.h:180`, copied at `nos/svc.c:994`), but the pager never reads it (`nos/apps/sys_home.c:474`). The faces get a 50 % battery that looks real (`nos/faces/face_matrix.c:278`, `nos/faces/face_split.c:78`). User impact: a fresh boot can show 50 % and the battery bars and status line can be wrong until the first stable reading.

2. **[medium] On B, the charging flag means USB host present, not charging.** B has no charge-status pin. `PIN_CHG_STAT` is defined only in the C block (`firmware/main/pins.h:33`, under `CONFIG_WATCH_BOARD_143C`). The read is guarded by `#ifdef` (`firmware/main/hal_esp32.c:324-326`). So B's `usb` is `usb_serial_jtag_is_connected()` only (`:323`), which the comment says is host SOF, not charge current. `external_power = usb ? 1 : -1` (`:337`), and `usb` alone sets `charging` (`:350-351`). The pager passes that as `ctx.charging` (`nos/apps/sys_home.c:475`) and uses it for the bolt on the status line (`nos/apps/sys_home.c:698`, `:819-822`), the face battery status (`nos/faces/faces.c:584-585`) and the SPLIT battery alarm (`nos/faces/face_split.c:110`). User impact: a wall charger with no PC attached may not be reported at all, and a PC attached with a full battery is reported as charging. Verify on device.

3. **[medium] A stale heart rate is shown as live.** `bpm_last` never expires. `heart_tick()` takes any history sample with no age check (`nos/svc.c:602-604`), and `svc_heart_record_at()` takes any reading with no age check (`:631-632`). The pager passes it on unchanged (`nos/apps/sys_home.c:473`). The SPLIT face shows it as HR and turns the value red at 120 or above or below 40 (`nos/faces/face_split.c:76`, `:109`, `:127-128`). User impact: an hours-old reading is shown as the current heart rate. This is the same defect as [[Heart rate]] (its gap 8).

4. **[medium] Notifications and control centre are reachable only from the face.** The vertical pull is gated on `nearest_page() == 0 && fabsf(H.x) < 40` (`nos/apps/sys_home.c:1088`). On widget pages and the Apps page a vertical drag falls through and does nothing (`:1094-1095`). User impact: from Music or Timer the user must first return to the face to reach notifications or the control centre.

5. **[medium, verify on device] Background recording does not keep the screen awake.** `svc_track.c`, `workout.c` and `imu_rec.c` make no `nos_keep_awake()` call (grep of the source). The sleep check uses only the top screen's `SCR_KEEPAWAKE`, a finger on the glass, and `nos_awake_ms()` (`firmware/main/nos_main.c:2078-2084`, `nos/nos.c:584-586`). Home's flags do not set it (`nos/apps/sys_home.c:1176-1177`), and the REC chip does not either (`:143-181`). User impact: the chip says REC or SPORT on the face, but the panel can sleep on `sleep_s` while the recording runs. Whether the sample streams keep going in the screen-off state is for [[Sports and workouts]] and [[Navigate]] to confirm.

6. **[low] The unread count is one dot, with no number and no tap target.** The dot is drawn at `(UI_CX, 24)`, radius 8 (`nos/apps/sys_home.c:986-992`). The tap handler has no zone for it (`:1109-1134`). User impact: the user cannot tell how many messages there are, and the dot does not open the list.

7. **[low] Page dots are not touch targets.** `draw_dots()` draws them (`nos/apps/sys_home.c:664-695`), and `home_input()` has no hit test at `UI_DOTS_Y` (`nos/ui.h:63`). Users cannot tap a dot to jump pages.

8. **[low] The next alarm is computed and never shown by the home face.** `ctx.next_alarm_min` is filled every frame (`nos/apps/sys_home.c:482`). No face reads it (`nos/faces/face.h:23`; a repo grep finds no reader). The alarm is shown only by the Timer widget's NEXT ALARM mode, when no timer or stopwatch is running (see [[Alarms]]). The computation costs a loop over up to 8 alarms each frame (`nos/svc.c:768-800`).

9. **[low] Weather freshness is dropped.** The pager copies `valid`, `temp_c` and `cond` only (`nos/apps/sys_home.c:477-480`). The service also carries `stale`, `source` and `age_seconds` (`nos/svc.h:70-71`), and the phone path sets `stale` and a "Phone weather is stale" status (`nos/svc.c:418-421`). The SPLIT face shows the temperature whenever `weather_valid` is set (`nos/faces/face_split.c:80-82`). User impact: a stale temperature appears current.

10. **[low] Steps freeze silently when the pedometer is invalid.** When `hal_activity` is invalid, `act_tick()` republishes the last maximum (`nos/svc.c:533`). `hal_activity_get()` marks the sample invalid after 250 ms (`firmware/main/board.c:495-505`). The pager shows the frozen number with no stale marker.

11. **[low] Haptics and sound are silent on a bare B, and the code reports them as present.** B has no motor and no speaker pin (`firmware/main/pins.h:35-67`; `BOARD_HAS_AUDIO 0` at `:64`). The haptic driver drives IO16 through LEDC (`firmware/main/ext/ext_haptic.c:105-109`). It sets the presence bit as soon as the LEDC driver starts, with no probe (`:107-109`, `nos/hal_ext.h:62-63`). The page tick (`nos/apps/sys_home.c:585`) and every tap (`nos/nos.c:570`) therefore do nothing on a board without the strap, with no indication.

12. **[low] Widget set and order cannot be changed.** The widget array is `const` (`nos/apps/sys_home.c:203`). `nos_prefs` has no page-order or visibility field (`nos/prefs.h:7-46`). A user cannot hide or reorder the five glance pages.

13. **[low] Per-frame cost with no visible benefit.** `build_ctx()` runs `svc_alarm_next()` and `svc_notif_unread()` every frame (`nos/apps/sys_home.c:465-490`). All five widgets tick every frame whether visible or not (`:626-636`).

Doc drift (docs that no longer match the code):

- `docs/polish/shell_r1.md:76-88` says the side button waits 320 ms on home for a possible double press, every time. The code dispatches `IN_BTN` on release at once (`nos/nos.c:973-975`). `DOUBLE_MS = 320` only sets the window in which a second press becomes `IN_BTN_DOUBLE` (`nos/nos.c:591`, `:968`).
- `docs/polish/shell_r1.md:136-139` says the face shows no unread indicator. The face draws one (`nos/faces/faces.c:522`, `:554-568`).
- `docs/polish/shell_r1.md:344-345` gives the dot row as `DOTS_Y 448`. The code uses `UI_DOTS_Y 440` (`nos/ui.h:63`).
- The header of `nos/apps/sys_home.c:1-24` matches the code on page order, the 1:1 drag, the cut for programmatic moves and the alias trip (`:402-407`).

## Related

- [[Services hub]]: the services the pager reads, and how `svc_tick` runs.
- [[Health and activity]]: the activity and heart sources.
- [[Sports and workouts]]: the workout chip and its recording state.
- [[Power and battery]]: the battery and charging sources, and the sleep timer.
- [[Storage and persistence]]: the prefs checkpoint and the NVS keys.
- [[Hardware and drivers]]: the B pin map, the RTC, the IMU, and the strap motor.
- [[Connectivity and voice]]: the phone feed for notifications and weather.
- [[Shell and faces]]: the faces, the picker and the control centre.
- [[Engine and scheduling]]: the frame loop, pacing, and the screen-off and AOD branches.
- Hosted pages: [[Activity]], [[Heart rate]], [[Weather]], [[Music]], [[Timer]], [[Alarms]], [[Navigate]], [[IMU]], [[Recorder]].
