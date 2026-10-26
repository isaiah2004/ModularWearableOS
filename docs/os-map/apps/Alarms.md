---
title: "Alarms"
id: app-alarms
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Alarms

## Summary

Alarms is the NOS app for setting, toggling and deleting alarms, and for the ringing screen. The app screens live in `nos/apps/time_alarm.c` (1095 lines): the list `scr_alarm` (`:584`), the editor `scr_alarm_edit` (`:903`) and the ringer `scr_alarm_ring` (`:1093`). It is launcher tile "Alarms" with `DOT_ICON_ALARM` (`nos/apps/registry.c:24`).

The data and the firing logic are not in the app. They live in the services layer, `nos/svc.c` (alarm block `:707-857`, Gadgetbridge sync `:1112-1137`), behind the `svc_alarm_*` API in `nos/svc.h:119-134`. Alarms fire, ring, snooze and time out inside `svc_tick()`, so they keep working after the app is closed (see [[Services hub]] and [[Engine and scheduling]]).

- **Storage**: one NVS blob, key `nosalarm`, namespace `wos`. Not SD, even though B has an SD slot (see [[Storage and persistence]]).
- **Hardware for the alarm itself**: a clock (RTC PCF85063 on B), the display and touch, and a vibration motor plus speaker on the external strap. The strap is not in the B pin map (see Background and Gaps).
- **Status: partial.** The list, editor, firing, snooze and persistence are all implemented end to end in source. Sound and buzz on a bare B are unverified, and several behaviours are rough (Gaps). This card is from source only, not a watch run.
- **Stale comment**: the header of `time_alarm.c:5-7` says the list falls back to a local table while svc is a "link stub". No stub exists in the tree, and the fallback is dead code in this build (Gaps).

## Features

- **List** (`scr_alarm`): title "ALARMS" (`time_alarm.c:362`). Up to 8 alarms (`SVC_ALARM_MAX`, `svc.h:119`), shown sorted by time of day (`time_alarm.c:230-236`).
  - Each row shows the time (`:438-442`), an AM/PM tag when the 12 h pref is on (`:443-444`), and a sub-label (`:447-453`).
  - The sub-label is `SNOOZED hh:mm` while snoozed (`:202-212`). Otherwise it is the day set from `fmt_days` (`:140-157`): `ONCE`, `EVERY DAY`, `WEEKDAYS`, `WEEKENDS`, or the weekday names.
  - A toggle on each row (`:454-459`). Tapping it flips `on` and saves (`:566-576`). Tapping the toggle of a snoozed row cancels the snooze and switches the alarm off (`:567`, `:507-512`).
  - Tapping the row body opens the editor (`:576`).
- **Header**: `NEXT IN 3H 5M`, `NEXT IN 2D 4H` or `NEXT IN 7 MIN` from the next alarm, or `ALL ALARMS OFF` (`:237-253`, `:134-139`). Changes crossfade (`:213-214`).
- **Empty state**: "No alarms" plus an ADD ALARM pill (`:363-372`).
- **Add**: an ADD row after the last alarm (`:419-431`). The default is 07:00, `ONCE`, on (`:496-503`). A full list gives the toast "List full" (`:498`).
- **Editor** (`scr_alarm_edit`, title "EDIT ALARM" or "NEW ALARM", `:774`):
  - Hour (0-23) and minute (0-59) drums (`:708-709`).
  - Seven weekday chips, Monday first (`:660-662`, `:742-747`, `:883-886`).
  - A summary line that names only ONCE, EVERY DAY, WEEKDAYS or WEEKENDS (`:675-680`).
  - An info line with an AM/PM tag (12 h pref) and `IN x` (`:759-761`).
  - DELETE: arm, then tap again within 3 s (`:596`, `:887-896`, `:735`). Confirm calls `ed_delete` (`:683-692`).
- **Ringing screen** (`scr_alarm_ring`, title "ALARM" or the alarm's label, plus AM/PM when 12 h, `:924-939`):
  - A big time readout (`:982-991`).
  - A SNOOZE pill (`:1000`). Tap it to snooze (`:1075-1083`).
  - A slide-to-stop track 260 px wide with a knob and the label "SLIDE TO STOP" (`:906-909`, `:1002-1013`). Committing the slide stops the alarm (`:1063-1067`, `:971-980`).
  - The side button (`IN_BTN`) snoozes, it does not stop (`:1085-1088`).
- **Snooze**: 9 minutes, fixed (`svc.c:48`, `:818-824`). A "Snoozed" toast shows (`time_alarm.c:1024`).
- **Auto-stop**: a ring ends by itself after 60 s (`svc.c:47`, `:848-851`).
- **Haptic and sound while ringing**: `hal_haptic(HAPTIC_ALARM)` plus `hal_sfx(SFX_ALARM)`, re-issued every 2 s (`svc.c:688-705`). `SFX_ALARM` loops until stopped (`firmware/main/ext/ext_sfx_synth.h:85-88`). Volume follows the global volume pref (`nos/apps/set_settings.c:1768`, `:2093`; `firmware/main/ext/ext_audio.c:337`).
- **Phone-set alarms**: Gadgetbridge `"alarm"` messages replace the whole list (`svc.c:1179`, `:1112-1137`). They are labelled `Phone` (`:1135`).

## Reads

Services (`nos/svc.h`, implemented in `nos/svc.c`):

- `svc_alarm_count()` (`svc.c:711`) and `svc_alarm_get(i)` (`:720`). The list reads the whole record (`time_alarm.c:210`, `:433`, `:568`, `:706`, `:741`).
- `svc_alarm_id(i)` and `svc_alarm_generation()` (`svc.c:712-713`). Used to detect that the list changed under an open editor or press (`time_alarm.c:491`, `:532-533`, `:626-628`).
- `svc_alarm_next(&h,&m,&in)` (`svc.c:768-801`), through `time_alarm_next` (`time_alarm.c:118-120`). It also feeds the Home face context (`nos/apps/sys_home.c:482`) and the Timer glance `NEXT ALARM` mode (`nos/apps/time_timer.c:1032`).
- `svc_alarm_snoozed(&idx, &at_ms)` (`svc.c:809-814`). Used for the SNOOZED label and its wall-clock minute (`time_alarm.c:101`, `:220-227`).
- `svc_alarm_ringing()` (`svc.c:802`). Used by the ring screen (`time_alarm.c:927`, `:956`, `:973`, `:1018`).

Prefs (`nos/prefs.h:18`; default `nos/prefs.c:147`):

- `nos_prefs.hour24`: 12 h or 24 h time (`time_alarm.c:443`, `:760`, `:936`). Formatting goes through `ui_fmt_time` (`nos/ui.c:901-905`).
- The app does not read `haptics` or `sound`. The ringer reads `sound` only for calls (`svc.c:701`).

HAL (`nos/hal_ext.h`, `nos/hal.h`):

- `hal_get_datetime` (`time_alarm.c:108`, `:222`, `:928`; `firmware/main/hal_esp32.c:142-156`, which reads `gettimeofday` and `localtime_r`).
- `hal_millis` (`time_alarm.c:223`; `hal_esp32.c:111`).
- `hal_haptic_stop` and `hal_sfx_stop` on leaving the ring screen (`time_alarm.c:951`).

Not the app's reads, but the same clock path: the RTC restore and the "clock unset" state (`firmware/main/board.c:840-890`).

## Writes

Services mutators (all in `nos/svc.c`):

- `svc_alarm_add` (`:743-753`). It also saves (`:751`).
- `svc_alarm_remove` (`:754-767`). It also saves (`:766`).
- `svc_alarm_save` (`:721-731`). It writes NVS key `nosalarm` through `hal_store_save_checked`.
- `svc_alarm_snooze` (`:818-824`), `svc_alarm_snooze_cancel` (`:816`), and `svc_alarm_stop` (`:825`).

The app mutates the svc record directly:

- `on`, `days` and `label` in `al_add` and `ed_commit` (`time_alarm.c:86-87`, `:644-646`).
- `on` on toggle and on snooze-cancel (`:510`, `:572`).
- `h`, `m` and `days` while the editor is open, straight into the svc record (`:748-749`, `:885`).
- `al_save` persists after each toggle (`:574`), after commit (`:647`), after delete (`:688`), and on editor leave (`:717`).

Svc also writes itself, with no UI involved:

- `alarm_tick` turns a one-shot alarm off when it fires, then saves (`svc.c:838`, `:841`).
- A phone sync rebuilds the whole list and saves (`:1116-1137`).

Diagnostics only: `diag_action_request/context/result` (`time_alarm.c:569-573`, `:974-977`, `:1019-1023`). The ledger writes to `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`). This is developer telemetry, not user data.

Local state only: the `A`, `E` and `R` structs (`time_alarm.c:167`, `:601`, `:912`) and the static `AL[]` fallback (`:61`). Nothing is written to SD or NVS by the app itself.

## Background behaviour

What keeps running, and who runs it:

- **Service tick.** `svc_tick()` runs from `services_tick` (`nos/apps/registry.c:62-65`), set as `nos_bg_tick` (`:71`). It is called once per frame from `nos/nos.c:2341` while the display is on. It calls `alarm_tick(now)` (`nos/svc.c:1326`) and `ringer_tick(now)` (`:1328`). Everything runs in the UI task.
- **Screen off.** The loop in `firmware/main/nos_main.c` calls `nos_bg_tick(1.0f)` at most once per 1000 ms (`:2091-2094`). The always-on display path runs one frame per second (`:2114-2117`), so the services run at 1 Hz there too. A new minute is caught on the first tick of that minute (`svc.c:831`), so an alarm starts within about 1 s of the minute boundary.
- **Snooze expiry and the 60 s auto-stop** are checked in the same `alarm_tick` (`svc.c:843-851`).
- **Ringing** is pushed by the service, not by the app: `ring_screen(&R_ALARM, ...)` (`svc.c:648`, `:659-676`, `:852`) calls `nos_push(&scr_alarm_ring, TX_RISE)`. The ring screen has `SCR_KEEPAWAKE` (`time_alarm.c:1094`). The main loop wakes the panel on a pending keep-awake screen (`nos_main.c:2096`, `:2121`), and the sleep policy holds while it is on top (`nos_main.c` sleep policy, which checks `SCR_KEEPAWAKE`).
- **Haptic and sound output** run on the strap modules. The motor is an LEDC channel on GPIO16 with an `esp_timer` callback (`firmware/main/ext/ext_haptic.c:102-109`, `:125`). The audio task `exta` is priority 3 (`firmware/main/ext/ext_audio.c:344`), and `hal_sfx` queues recipes to it (`:358-364`).
- **Closed app.** With the list closed, the ringing still happens, because the ring is a svc event (`nos/svc.h:118`, comment "keep running while their apps are closed"). The list and editor screens only tick while on top.
- **Phone sync** arrives through the phone line parser (`svc.c:1179`), so a phone change can land while the screen is off.

Naming: `alarm_tick` exists twice. `time_alarm.c:296` is the screen tick and `svc.c:826` is the service tick. Both are file-local statics, so there is no clash.

## Persistence

- **Key and namespace.** NVS namespace `wos`, key `nosalarm` (`firmware/main/hal_esp32.c:177-183` for save, `:277-283` for load). `hal_store_save_checked` routes every key except `nosprefs` and `wifi_cred` to `nvs_save` (`hal_esp32.c:228-233`). Alarms are therefore not on SD, even though B sets `BOARD_HAS_SD` to 1 (`firmware/main/pins.h:63`).
- **Blob.** `alarm_saved` is `{ version=1, count, pad[2], svc_alarm a[8] }` (`svc.c:708-709`). With `svc_alarm` at 20 bytes (`svc.h:120`), that is 164 bytes.
- **Load.** `svc_init` calls `alarm_load` (`svc.c:1288`, `:732-742`). If the version or count is wrong, the list is emptied with no message (`:739`). Ids are rebuilt each boot (`:740`).
- **Save points.** Add (`svc.c:751`), remove (`:766`), a one-shot firing (`:841`), a toggle, commit, delete and editor leave (`time_alarm.c:574`, `:647`, `:688`, `:717`), and a phone sync (`svc.c:1137`).
- **Not persisted.** The snooze (`s_snooze_idx`, `s_snooze_at`, `svc.c:86-90`) and the ring (`s_ring_alarm`, `:86`). A reboot during a snooze forgets it.
- **Save result.** `svc_alarm_save` only reports success or failure to the diagnostics ledger (`svc.c:728-731`). Nothing retries and nothing shows the user (see Gaps).
- **Clock.** Alarm times are wall-clock (`localtime_r`). The RTC restores the system clock at boot (`firmware/main/board.c:847-`). If neither the RTC nor a phone sets it, the clock is the epoch (`board.c:882-887`, `tv_sec = 0`).
- **Prefs.** `nos_prefs.hour24` is stored as `nosprefs` on SD under `/sdcard/watch`, mirrored to NVS (`firmware/main/hal_esp32.c:236-251`).

## Navigation

Opened from:

- The launcher tile "Alarms" (`nos/apps/registry.c:24`).
- The Timer glance in `WG_ALARM` mode. It is chosen when `time_alarm_next` returns an alarm (`nos/apps/time_timer.c:1032`), and a tap opens `scr_alarm` (`:1195`).
- A ring: `scr_alarm_ring` is pushed by the service (`svc.c:659-676`), not by any tap.

Pushes and pops:

- The list pushes the editor with `nos_push(&scr_alarm_edit, TX_PUSH)` (`time_alarm.c:494`). This is used for both edit and add (`:496-503`).
- The editor pops itself on delete (`:691`) and when the alarm disappears underneath it (`ed_reconcile`, `:630-637`).
- The ring pops itself when the service clears the ring (`:956-958`). Slide-to-stop pops after the stop (`:979`). Snooze pops after the snooze (`:1025-1026`). Swiping or covering the ring stops the alarm (`svc.c:852`, see Gaps).

The ring is `SCR_NOBACK` (`time_alarm.c:1094`), so the back gesture cannot leave it.

Home face: `sys_home.c:482` fills `next_alarm_min` in the face context, but no face in `nos/` reads it (`nos/faces/face.h:23`).

## Gaps

- **Sound and buzz on B depend on an unverified external strap.** `EXT_SPEAKER` and `EXT_HAPTIC` are set without any probe (`firmware/main/ext/ext_audio.c:343`, `ext_haptic.c:109`). The strap modules are compiled in on B (`firmware/main/ext/ext_sys.c:177`, `ext_priv.h:26-30`). The motor is on GPIO16 and the speaker on I2S 1/2 and amp 5/6 (`ext_priv.h:33-38`). The B pin map has no built-in audio (`firmware/main/pins.h:64`), and `CLAUDE.md` marks the strap mapping as unverified. On a bare B the screen wakes and rings but nothing sounds, and nothing tells the user. Severity high.
- **Clock may be unset, with no warning.** An unset RTC and no phone sync leave the epoch clock (`board.c:882-887`). The alarm app never checks `board_clock_valid()` (`board.c:840`). Its only caller is the connectivity state (`firmware/main/hal_esp32.c:905`). A user can set 07:00 and nothing says it will not fire at the right time. Severity medium.
- **Failed alarm saves are silent.** `svc_alarm_save` reports a failed `hal_store_save_checked` only to the diagnostics ledger (`svc.c:728-731`). There is no retry and no toast, and the app ignores the result (`time_alarm.c:97`). The list shows the change, but it is lost on reboot. Severity medium.
- **Two alarms at the same minute: only the last rings.** `alarm_tick` calls `alarm_ring(i)` for each match (`svc.c:834-840`). Each call overwrites `s_ring_alarm` (`:803-808`). The earlier alarm never rings, and if it was a one-shot it is still switched off (`:838`). The snooze then covers only the last one. Severity medium.
- **A phone sync wipes on-watch alarms.** `gb_alarms` zeroes `s_alarms` and rebuilds the list from the phone (`svc.c:1116-1137`). On-watch alarms and edits are lost on every sync, with no merge and no indication. Severity medium.
- **Snooze cancel turns the whole alarm off.** Tapping the toggle of a snoozed row sets `on = 0` (`time_alarm.c:507-512`). For a recurring alarm that cancels every day, not just the snooze. Severity medium.
- **Swiping or covering the ring stops it.** `ring_screen` returns 1 when a non-ring screen covers it, and `ring_tick` treats that as a stop (`svc.c:659-676`, `:852`). A stray cover kills the alarm instead of snoozing it. Severity medium.
- **No labels can be set.** The editor never writes `label`. The draft label is zeroed (`time_alarm.c:500`) and commit copies it (`:646`). The ring then shows "ALARM", or "Phone" for synced alarms (`:930`, `svc.c:1135`). Severity medium.
- **Snooze state is RAM only.** A reboot during the 9 minutes forgets the snooze and the one-shot is already off (`svc.c:86-90`, `:838`). Severity low.
- **Fixed behaviour with no settings.** Snooze is 9 min (`svc.c:48`). Auto-stop is 60 s (`:47`). There are no per-alarm sounds or vibration patterns, no gradual wake, no skip-next, and no separate alarm volume. Severity low.
- **Haptics and sound prefs do not apply.** `ringer_tick` vibrates and sounds for alarms regardless of `nos_prefs.haptics` (`svc.c:699`), and skips the sound pref for alarms (`:701`). Severity low.
- **Editor shows 24 h hour digits in 12 h mode.** `tdrum_draw` formats the value with `%02d` (`nos/apps/time_timer.c:302`), and `time_alarm.c:708` sets the range 0-23. The list uses `ui_fmt_time`, which honours the pref (`nos/ui.c:901-905`). The two screens disagree. Severity low.
- **Edits to an existing alarm live only in RAM until the editor closes.** Hour, minute and days are written straight into the svc record (`time_alarm.c:748-749`, `:885`). A power loss in the editor loses them (`:717`). Severity low.
- **Each add or delete writes NVS twice.** `svc_alarm_add` saves before `ed_commit` sets the days (`svc.c:751`, `time_alarm.c:642-647`). A delete saves in `svc_alarm_remove` and again in `ed_delete` (`svc.c:766`, `time_alarm.c:688`). A power loss between the two leaves a one-shot. Severity low.
- **Dead fallback and stale comment.** The `AL[]` table and `a_svc == 0` branches (`time_alarm.c:61-97`, `:118-131`) are reached only when `svc_alarm_add` fails with room left. The UI only sends valid 0-23 and 0-59 values (`:708-709`), so this does not happen. The header at `:5-7` describes a link stub that is not in the tree (`svc_alarm_add` is only at `svc.c:743`). Severity low.
- **Home next-alarm field is never read.** `next_alarm_min` is filled at `nos/apps/sys_home.c:482` and never read by a face (`nos/faces/face.h:23`). Severity low.
- **Not on this board's hardware map.** The motor and speaker strap pins are in `firmware/main/ext/ext_priv.h`, not in `firmware/main/pins.h`. The B target has no pin for them in the workspace map (see [[Hardware and drivers]]). Severity medium.
