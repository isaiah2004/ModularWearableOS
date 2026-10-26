---
title: "Tape face"
id: face-tape
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Tape face

Source: `nos/faces/face_tape.c` (registry index 7, `nos/faces/faces.c:15-16`), with shared helpers in `nos/faces/face_kit.h` and `nos/faces/faces.c`. Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from source reading only, not from the device.

## Summary

TAPE is a dot-matrix "cassette" face. Two dot-grid reels sit in a cassette window: the left (supply) pack empties and the right (take-up) pack fills across the day. A dot-matrix timecode `HH:MM:SS` sits below the window, and a date with a red blinking lamp sits above it.

The face is presentation only. The shell builds one `face_ctx` per frame (`nos/apps/sys_home.c:465-490`) and the face reads from it. TAPE uses four inputs: the clock, battery and charging state, and the unread count. It has no tap target and writes nothing persistent. Its only RAM state is animation bookkeeping (`nos/faces/face_tape.c:35`).

- Purpose, as the file header states it (`nos/faces/face_tape.c:1-16`): a recorder face where the reels hold the day and the lamp is the recording light. The reels and timecode do what the header says. The lamp does not (see Gaps).
- Hardware needed: the 466 x 466 panel through the frame pipeline, the system clock (restored from the PCF85063 RTC at boot), the battery reading from `hal_power_get()`, and PSRAM for the reel dot cache. The face itself uses no IMU, touch target, audio, haptic, Wi-Fi or SD.
- Status: working as a presentation face, at source level. It is built into any firmware that compiles `nos/*.c`, via `firmware/main/CMakeLists.txt:15`.

Related: [[Shell and faces]] (page 0 host and face registry), [[Face picker]], [[Face registry and shared face kit]], [[Engine and scheduling]] (tick and frame loop), [[Services hub]] (the `svc_*` getters behind the face context), [[Power and battery]] (battery status, AOD), [[Hardware and drivers]] (panel, RTC, battery ADC), [[Storage and persistence]] (prefs checkpoint), [[Connectivity and voice]] (unread notifications, the REC chip sources), [[Health and activity]] and [[Sports and workouts]] (the shell gathers their values for every face, TAPE does not use them).

## Features

- **Two reels that split the day.** Each reel has a 40-dot rim at radius 70 and four pack rings at radii 28, 38, 48 and 58, with 16 + 22 + 27 + 33 = 98 pack positions (`nos/faces/face_tape.c:25-28`, `:128-139`, `:145-146`). The lit count is `lit_l = round((1 - day) * 98)` and `lit_r = 98 - lit_l`, where `day` is minutes since midnight over 1440 (`face_tape.c:203-207`). The 98 positions are one shared pool, so a dot changes reels about every 14.7 minutes (1440 / 98). The header says "about one dot per 8 minutes" (`face_tape.c:9`, `:18-19`, `:204-205`). That is not what the code does.
- **Hub stepper.** Each hub is a 12-slot ring at radius 15 with slot 0 empty, so 11 dots show (`face_tape.c:29`, `:179-189`). Once a second the base angle advances 30 degrees, drawn over four frames at 40/72/92/100 % (`face_tape.c:48-59`, `FK_TICK4` at `nos/faces/faces.c:69`).
- **Timecode.** `HH:MM:SS` in the tall dot font, 6 px pitch, baseline y 322 (`face_tape.c:30-31`, `:230`). It shows 24 h by default because `nos_prefs.hour24` defaults to 1 (`nos/prefs.c:147`). With the pref off it shows 12 h, e.g. `9:41:13` (`nos/faces/faces.c:85-100`). Seconds change through a 4-frame dot morph (`face_tape.c:249`). A width change, such as 9:59:59 to 10:00:00, is a clean cut (`face_tape.c:246-247`).
- **Date.** `SUN 11`: three-letter weekday and day of month, cross-faded over 5 frames when it changes (`nos/faces/faces.c:107-110`, `face_kit.h:60-61`, `faces.c:162-169`). There is no month or year.
- **Recording lamp.** A 9 px red disc left of the date, pulsing on the shared once-a-second blink envelope (`face_tape.c:251-267`, `faces.c:801-811`). It is drawn whenever the face is awake and not in AOD. It does not read recording state (see Gaps).
- **Cassette window.** A 2 px rounded outline, radius 40, spanning x 46..420 and y 118..290 (`face_tape.c:226`).
- **Battery and charging line.** At y 406. Shown only while charging, or at 20 % and below. A bolt appears while charging (`face_tape.c:268-271`, `faces.c:582-599`). In AOD it is shown only below 10 % and not charging (`faces.c:584`).
- **Unread dot.** 8 px at (UI_CX, 24) in a neutral mix of secondary and primary ink, never red on this face (`face_tape.c:272`, `faces.c:565-575`). It is a 6 px C_SEC dot in AOD (`faces.c:567-569`).
- **Wake from AOD.** Over 6 frames the rims and hubs grow, the pack dots fade up, HH:MM scrolls left in whole 6 px cells, the date steps 2 px per frame and the lamp comes up (`face_tape.c:11-12`, `:60-70`, `:225`, `:257-266`; `faces.c:455-489`).
- **Midnight rewind.** When an awake, non-calm tick sees 23:59 followed by 00:00, an 11-frame rewind starts (`face_tape.c:86-89`, `:32`). Dots move from the take-up reel back to the supply, and both hubs turn back one pitch per frame (`face_tape.c:48-59`, `:210-214`). The timecode morph runs with it.
- **AOD face.** HH:MM only, with no seconds. Rims and hubs show dim. There are no pack dots and no lamp. The date is dim. Burn-in drift is up to 4 px, one step per minute (`face_tape.c:110`, `:160`, `:231`, `:257`, `:270`; `faces.c:19-32`, `:57-64`).
- **Build-in.** The reels grow outward over 8 frames (`face_tape.c:217-218`). The whole face builds in over `ENTER_S` = 0.40 s, which is 12 frames (`nos/apps/sys_home.c:55`, `nos/faces/face_kit.h:30-31`).

## Reads

The shell builds `face_ctx` (`nos/faces/face.h:14-27`) once per frame in `build_ctx` (`nos/apps/sys_home.c:465-490`). TAPE reads these fields:

- `c->dt` (hour, minute, second, day, weekday, millis), latched from `hal_get_datetime()` (`sys_home.c:468`). That reads the ESP-IDF clock through `gettimeofday` and `localtime_r` (`firmware/main/hal_esp32.c:142-146`).
- `c->battery`, from `svc_sys()->battery_pct`, falling back to `hal_battery_percent()` (`sys_home.c:474`; `nos/svc.c:986-993`; `hal_esp32.c:392`).
- `c->charging`, from `svc_sys()->charging` (`sys_home.c:475`). On this board that is `external_power == 1` (`svc.c:992`), which is derived as described in Gaps.
- `c->unread`, from `svc_notif_unread()` (`sys_home.c:476`; `nos/svc.c:238-242`).
- `c->t` (= `nos_t`), `c->aod` (= `nos_aod` and not dimming out), and `c->enter` (build-in progress) (`sys_home.c:483-485`).
- `c->lite` through `fk_ghost()`, which fades the unlit detail while the picker dims the face (`nos/faces/face_kit.h:141-146`).

Face helpers it calls, all in `nos/faces/faces.c`: `fk_aod_update` (`:57-64`), the wake functions (`:458-489`, using `hal_millis()` at `:460`, defined at `firmware/main/hal_esp32.c:111-113`), `fk_unread_tick` (`:552-560`), `fk_time` and `fk_hh` (`:85-100`), which read `nos_prefs.hour24` (`:81`, `:89`), `fk_date` (`:107-110`, with `ui_weekday3` at `nos/ui.c:914-918`), `fk_status_shown` and `fk_status` (`:582-599`), `fk_blink` and `fk_sec_k` (`:50-55`, `:801-811`), and the frame tables (`:68-69`).

Prefs: TAPE reads only `nos_prefs.hour24`. It does not read `face`, `face_accent`, `theme`, `dnd` or `aod_bright`.

Fields of `face_ctx` that TAPE does not read: `steps`, `step_goal`, `bpm`, `weather_valid`, `temp_c`, `wx_cond`, `next_alarm_min` and `rec` (`face.h:14-27`). `build_ctx` still calls `svc_activity()`, `svc_heart()`, `svc_weather()` and `svc_alarm_next()` on every tick for every face (`sys_home.c:469-482`), so TAPE pays for reads it ignores.

The face's own source has no `svc_*`, `prefs_*` or `hal_*` call (checked by grep of `face_tape.c`). Those reads reach it only through `build_ctx`.

Hardware it depends on:
- The panel and frame pipeline (`firmware/main/pins.h:36-66`, B block: CO5300 QSPI, 80 MHz).
- The system clock. It is restored from the PCF85063 RTC at boot (`firmware/main/board.c:871-874`, RTC at `pins.h` B block).
- Battery and charge state from `hal_power_get()` (`firmware/main/hal_esp32.c:378-388`). The sample task refreshes it, and `sys_tick()` copies it into `svc_sys()` once a second (`nos/svc.c:986-993`, `:1315-1317`).
- PSRAM for the reel dot cache (`nos/faces/faces.c:242`). If that allocation fails, dot lists are evaluated directly on every pass (`faces.c:237-244`, `:298-349`).

## Writes

Face state is one static struct, all RAM (`nos/faces/face_tape.c:35`):
- `S.tc` (timecode morph), `S.w` (wake state), `S.date` (date cross-fade).
- `S.last_hm`: the last minute seen, starts at -1 (`face_tape.c:35`, `:90`).
- `S.rew_t0`: the midnight rewind start, -100 for none (`face_tape.c:88-89`).
- `S.pj` and `S.pa`: probe-only, used when `GFX_COST_MODEL` is defined (`face_tape.c:93-124`). That is a simulator motion audit and is compiled out of the watch build.

`enter()` resets `S.tc.init` (`face_tape.c:37`). The shell calls it through `sys_home_build_in()` (`nos/apps/sys_home.c:278-285`). That runs on the first home init (`:437`), on wake (`:547`), and at boot through `nos/apps/sys_boot.c:108`.

Shared state the face changes as side effects (`nos/faces/faces.c`):
- `fk_aod_dx` and `fk_aod_dy`, the burn-in shift (`:34`, `:62-63`).
- `s_sk`, the second tracker behind `fk_sec_k` (`:40-49`), and `s_dc_epoch` (`:35`, `:59`).
- `s_unread_k`, the unread-dot ink step, shared by every face (`:548`, `:552-559`).
- The reel dot cache `s_dc` (`:225`, `:238-244`). It is a PSRAM allocation tried once per boot.
- `face_segment_warm()`, called every awake frame while the build-in is done (`:61`). It warms SEGMENT's scaled font and sprites in PSRAM (`nos/faces/face_segment.c:323-334`). So TAPE on the glass also does SEGMENT's one-time warm-up work.

Output: the framebuffer only (`dot_disc`, `dot_text*`, `ui_rrect_line`, `ui_text`), inside `draw(gfx_fb *, const face_ctx *)` (`face_tape.c:197`).

Not written: no `prefs_*` call, no `svc_*` mutator, no notification read or dismiss, no SD or NVS, no haptic, sound, alarm or touch target.

## Background behaviour

The face has no task, timer, ISR or queue of its own. The shell ticks and draws it (`nos/apps/sys_home.c:625` in `home_tick`; draws at `:901`, `:913` and `:953`). The shell runs from the UI loop, which the frame pipeline runs on core 0 (`firmware/main/nos_main.c:1-5`; `nos/nos.c:2101-2110`, `:2347`).

| Device state | What runs | What TAPE does | Evidence |
|---|---|---|---|
| Awake, face on the glass (PWR_ON) | UI loop each frame. Authored animation clock is `NOS_FPS` 40. Presentation cap is a separate pref, default 60. | Full animation: hub step each second, timecode morph, lamp blink, pack change each minute, midnight rewind | `nos/render_config.h:8`, `nos/motion.h:25`, `nos/frame_cap.h:5`, `nos/prefs.c:161`, `face_tape.c:72-125` |
| Face picker open | The picker ticks and draws the face | Same as awake | `nos/apps/sys_faces.c:155`, `:284-286` |
| Apps page or widget page covers the glass | The face is not ticked while the Apps page covers it | No tick, so no morph progress and no rewind | `sys_home.c:623-625` |
| AOD (PWR_AOD or PWR_SENSOR) | After the entry ramp, one frame per second, with `nos_aod = 1` | AOD branch: HH:MM, dim rims and hubs, no hub motion, no lamp, no packs. Rewind is blocked by `calm`. | `firmware/main/nos_main.c:2106-2121`, `face_tape.c:44`, `:82`, `:89`, `:110` |
| Screen off (PWR_OFF) | Loop at 20 ms with no `nos_frame_run`, so no face tick or draw. `nos_bg_tick` runs once a second. | Nothing runs. Face state freezes. On wake, a gap over 500 ms counts as stale: morphs settle and no rewind replays. | `nos_main.c:2091-2100`, `:2094`; `nos/apps/registry.c:62-66`; `nos/faces/faces.c:458-461`; `face_tape.c:81`, `:84-85`, `:88` |

The services that feed the face run whether or not the face is visible. `svc_tick` runs every UI frame with the screen on, or once a second with it off (`nos/svc.c:1303-1320`). Its slow block calls `sys_tick()` once a second, which refreshes battery and charging (`svc.c:1315-1317`, `:986-993`). `svc_notif_unread()` is a RAM scan run on each shell tick (`svc.c:238-242`).

Per-second cost, from the source comment and not measured in this review: the hubs move for 4 frames, the seconds morph runs 4 frames, the lamp pulses, and the pack dots change about once every 15 minutes. The author counts a full redraw at about 300 small dots (`face_tape.c:18-19`). That matches 2 reels x (40 rim + 98 pack + 12 hub).

## Persistence

TAPE persists nothing. Its state is RAM only (`face_tape.c:35`). It starts empty at boot and is lost on reboot. That means the first midnight after a boot cannot rewind until the face has seen 23:59 once.

Shared settings it relies on:
- `nos_prefs.face` = 7 selects TAPE (`nos/prefs.h:20`; `nos/apps/sys_home.c:271-276`). The picker sets it when it closes (`nos/apps/sys_faces.c:372-375`) and calls `prefs_save()`.
- `nos_prefs.hour24`, default 1 (`nos/prefs.c:147`).
- Prefs are checkpointed to SD as `/sdcard/watch/settings.{0,1}.bin` (`nos/prefs_disk.c:32`; `firmware/main/hal_esp32.c:243`). NVS is the mirror for card-absent boots (`hal_esp32.c:249-250`). The write path is `prefs_tick` through `hal_store_save_checked("nosprefs")` (`nos/prefs.c:349`, `:366`). The SD file names are in `firmware/main/sd_xfer.c:294`.
- The unread count is RAM only. `svc_init` zeroes `s_nn` (`nos/svc.c:1285`), so the dot stays off after a reboot until new notifications arrive.
- The clock comes from the RTC at boot (`firmware/main/board.c:871-874`). When neither the RTC nor a sync is available the system clock is set to epoch zero (`board.c:885-886`).

## Navigation

Reached from:
- **Home pager, page 0.** Physical order is Activity, Heart, Weather, Music, Timer, Face, Apps (`nos/apps/sys_home.c:3-8`). Page 0 draws `nos_faces[nos_prefs.face]` (`sys_home.c:271-276`).
- **Face picker.** A long press on the home face opens it in place (`sys_home.c:1134-1141`, `nos_push(&scr_faces_home)`). TAPE is page 7 (`nos/faces/faces.c:15-16`). See [[Face picker]].
- **Settings.** The "Watch face" row pushes `scr_faces` (`nos/apps/set_settings.c:1852`).

Opens from TAPE:
- Nothing from the face itself. `tap` is 0 in the face record (`nos/faces/face_tape.c:275`). `nos_face_press_rect` therefore returns 0 (`nos/faces/faces.c:830-840`), so there is no press highlight, and a tap on the glass opens nothing (`nos/apps/sys_home.c:1126-1127`).
- The REC chip is the only tappable item on the page while a recording is up. It sits at y 406 as words only (`sys_home.c:189-192`, `:885-892`). A tap on it opens `sys_rec_screen(kind)` (`sys_home.c:1119-1121`, `:174`). Kind 4 is the workout, 3 the IMU log, 2 the memo, and anything else the track screen `scr_nav`. Those kinds come from the state rules in `sys_home.c:150-173`.
- Pager neighbours: widgets to the left, Apps to the right (`sys_home.c:3-8`).

## Gaps

1. **[medium] The red "recording lamp" is not tied to recording.**
   Evidence: `nos/faces/face_tape.c:9-10`, `:251-252`, `:264-266`. The lamp pulses from `fk_blink` and never reads `c->rec` or `sys_rec_state()`. Recording state is defined at `nos/apps/sys_home.c:150-173`. The TAPE chip has no dot, words only (`sys_home.c:189-192`). `nos/faces/face_kit.h:135-137` says the lamp "is the recording light". The 2026-10-21 audit already calls it decorative (`docs/audits/2026-10-21/report.txt:5197-5214`, FACE-011).
   Impact: an idle TAPE face blinks red all day, which reads as a recording in progress. While a memo, IMU log, workout or track is recording, the only signal is the chip's words, with no red dot.

2. **[medium] No complications and no tap targets. Data the shell gathers for it goes unused.**
   Evidence: `face_tape.c:275` (tap 0), `nos/faces/faces.c:830-840`, `nos/apps/sys_home.c:469-482` (steps, goal, heart rate, weather and next alarm gathered every tick). TAPE reads none of them (`face.h:14-27`).
   Impact: the face shows no steps, heart rate, weather, next alarm or timer, and tapping it does nothing. The wearer gets no glance at health or upcoming items, and no path into an app.

3. **[medium] Battery level is hidden above 20 %.**
   Evidence: `nos/faces/faces.c:582-586` (shown only while charging, or at 20 % and below; AOD only below 10 % when not charging), `face_tape.c:268-271`.
   Impact: there is no always-visible charge figure. The wearer sees charge only once it is low.

4. **[medium] On the 1.43-B the charging bolt is inferred, not sensed (not verified on hardware).**
   Evidence: `firmware/main/pins.h:36-66` (B block) has no `PIN_CHG_STAT`. Only the 1.43C block defines it (`pins.h:33`). `firmware/main/hal_esp32.c:323-325` compiles the STAT check out on B. `hal_esp32.c:337` sets `external_power = usb ? 1 : -1`, with `usb` taken from the USB-serial host (SOF) only. `hal_esp32.c:347-352` also sets 1 when the rail reads 4400 mV or more for three samples. `hal_esp32.c:287` starts `battery_percent` at a placeholder of 50 until samples settle.
   Impact: the bolt appears on B only when a USB host is attached or the rail reads high. A wall-charged watch with no host may show no bolt.

5. **[low] Doc and comment claims disagree with the code, and some docs are stale.**
   Evidence: `face_tape.c:9`, `:18-19`, `:204-205` claim one dot per about 8 minutes and about 180 dots per reel. The code has 98 shared positions (`face_tape.c:128-139`, `:206-207`), which is one dot per about 14.7 minutes. `face_tape.c:6-7` says the hub steps in 4 frames. `:18-19` says 6 frames, while `FK_TICK4` has 4 entries (`faces.c:69`).
   Stale docs: `docs/polish/faces_r1.md:73-76` says TAPE has no battery state, but `face_tape.c:268-271` draws one. `:142-155` describes ring-only packs of about 155 dots with a 4 px C_LINE. The code lights single dots, and unlit dots are 3 px in C_LINE2 (`face_tape.c:161-175`). `docs/os-map/systems/Shell and faces.md:94` calls TAPE "clock only", which is wrong.
   Impact: anyone tuning from these docs will chase the wrong numbers.

6. **[low] No month or year on the date.**
   Evidence: `nos/faces/faces.c:107-110` formats `"%s %d"`: weekday and day only.

7. **[low] The midnight rewind runs only when the face sees 23:59 and then 00:00 while awake and on the glass.**
   Evidence: `face_tape.c:86-89` needs `S.last_hm == 1439` on the previous tick and not calm. `S.last_hm` is RAM and starts at -1 (`face_tape.c:35`). There is no tick with the screen off (`firmware/main/nos_main.c:2091-2100`), none while the Apps page covers the glass (`sys_home.c:623-625`), and none in AOD (`face_tape.c:82`).
   Impact: if the screen was off, or Apps covered the face at midnight, the packs jump with no rewind. This is cosmetic only.

8. **[low] The clock is not validated.**
   Evidence: `sys_home.c:468` and `firmware/main/hal_esp32.c:142-146` pass the clock through with no validity check. `firmware/main/board.c:885-886` sets epoch zero when the RTC is unset. The only "UTC SYNCED" status is on the modules screen (`nos/apps/set_modules.c:777`).
   Impact: an unset watch shows an epoch-zero date and time with no warning.

9. **[low] AOD carries no day progress and no lamp, by design.**
   Evidence: `face_tape.c:160` (pack dots have `d = 0` when `k <= 0`), `:231` (AOD time branch), `:257` (lamp off in AOD).
   Impact: in AOD the reels convey nothing. This looks like a decision, not an accident, so it is recorded here for review.

10. **[low] The unread count does not survive a reboot.**
    Evidence: `nos/svc.c:238-242` counts unread entries in RAM. `svc_init` zeroes `s_nn` (`:1285`). The dot is drawn at `face_tape.c:272`, and it is not tappable (`faces.c:565-575`, no tap target).
    Impact: after a reboot the dot stays off until new notifications arrive, even if messages are still unread on the phone.
