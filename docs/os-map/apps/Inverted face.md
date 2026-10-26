---
title: "Inverted face"
id: face-inverted
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Inverted face

Source: `nos/faces/face_inverted.c` (177 lines). Read-only source review; nothing built, flashed or run on a board, and the serial port was not touched. Evidence is `repo-relative/path:line`. Target: 1.43-B.

## Summary

INVERTED is the seventh of nine faces in the picker, array index 6 (`nos/faces/faces.c:15-16`). It is the "white phone" face: a full white disc with a black hairline ring at r = 220 (`face_inverted.c:29`, `:118-121`), dot-matrix HH:MM in black, a red seconds dot riding the ring, a date above the time and a step count below it (`:136-150`). Its AOD variant is dark, with the time as grey ring-dots (`:97-108`).

The face owns no sensor, no storage and no settings. The home screen builds one `face_ctx` per frame (`nos/apps/sys_home.c:465-491`) and the face draws from it. The only direct service call in the file is `svc_activity()` as a steps fallback (`face_inverted.c:76`). Tap is limited to the steps line, which opens Activity (`:165-174`).

Status is "working" from source reading. The clock, date, steps and battery paths are real and the file has no placeholders. Nothing was measured on hardware in this pass. The gaps below are data-quality and feature gaps, not missing code paths.

## Features

- **Disc and ring.** Full white disc grown from the centre over 10 frames on `EASE_STEADY` (`face_inverted.c:34`, `:44-49`, `:118`). Black 2 px hairline at r = min(r, 220) (`:120-121`).
- **Time.** Dot-matrix "HH:MM", 12 h or 24 h from `nos_prefs.hour24` through `fk_hh` (`nos/faces/faces.c:85-90`, `:79-83`). A minute change morphs over 8 frames (`face_inverted.c:36`, `:71`, `:134`). During the opening the digits are mid-grey (`:33`, `:126`) and darken to black in 3 steps (`:51`).
- **Ghost cells.** Unlit dot-matrix cells in #EFEFEF on white (`:31`, `:128`). They are gated by the opening fade, so none are drawn on black (`:50-53`, `:127-128`). In the picker they fade out with `fk_ghost` (`nos/faces/face_kit.h:141-146`).
- **Date.** "SUN 11" from `ui_weekday3` and `dt.day` (`faces.c:107-110`), drawn at y 132 with a 5-frame cross-fade when the string changes (`face_inverted.c:74-75`, `:139`).
- **Battery status.** Mono "NN%" at y 158, with a bolt when charging. Shown only when charging, or when the reading is 1 to 20 % (`faces.c:582-600`, `face_inverted.c:140`). AOD shows it only below 10 % and not charging (`faces.c:584`).
- **Steps line.** "N STEPS" below 10,000, then "N.NK STEPS" (`face_inverted.c:141-142`), at y 344 (`:143`). It fades in with the opening (`:136-137`).
- **Seconds.** A 5.5 px red dot on the ring at angle (second + millis/1000) / 60 x 2 pi (`:146-150`). Redrawn every awake frame, which is 40 Hz (`nos/render_config.h:8`). Turns to ink when recording (`face.h:20`, `face_kit.h:151-155`).
- **Unread dot.** 8 px at (233, 48), i.e. dy 24 added to y 24 (`face_inverted.c:153`, `faces.c:565-572`). It fades in 4 steps and is ink on white. During the opening it is mid-grey (`:154-161`).
- **Wake.** Leaving AOD re-runs the opening, with the time kept from AOD and no reveal (`:60-66`, `:131-134`).
- **AOD.** Ring in grey (`:100`), hollow grey ring-dots for the time (`:101-104`), date at y 132 and status at y 158 (`:105-106`), unread dot 6 px (`faces.c:569`). Everything shifts by up to 4 px per minute (`nos/faces/face.h:6-7`, `faces.c:57-63`). No seconds and no steps in AOD.

## Reads

Direct calls in `face_inverted.c` (verified by grep): `svc_activity()` (`:76`), `fk_*` helpers, `dot_*`, `gfx_*`, `ui_*`, `bezier_ease`, `m_clamp`, `snprintf`. No direct `prefs_*` or `hal_*` call.

Through `face_ctx` (`nos/faces/face.h:13-27`), filled by `build_ctx` (`nos/apps/sys_home.c:465-491`):

| Field used | Filled from | Evidence |
|---|---|---|
| `dt` (hour, minute, second, millis, day, weekday) | `hal_get_datetime` | `sys_home.c:468`; `firmware/main/hal_esp32.c:116-146` |
| `steps` | `svc_activity()->steps`, else `svc_sys()->steps_today` | `sys_home.c:471`; `nos/svc.c:440-456` |
| `battery` | `svc_sys()->battery_pct`, else `hal_battery_percent()` | `sys_home.c:474`; `svc.c:987-993` |
| `charging` | `svc_sys()->charging` (`external_power == 1`) | `sys_home.c:475`; `svc.c:991-992` |
| `unread` | `svc_notif_unread()` | `sys_home.c:476`; `nos/svc.c:238-243` |
| `aod` | `nos_aod && !aod_dimming_out()` | `sys_home.c:484` |
| `enter` | home build-in clock `H.enter_t0`, 0.40 s; `sys_home_build_in()` also calls the face's `enter` hook | `sys_home.c:485`, `:278-284` |
| `t` | `nos_t` (engine clock) | `sys_home.c:483` |
| `rec` | `H.rec_raw`, for red-to-ink on recording | `sys_home.c:488`; `face_kit.h:151-155` |
| `lite` | picker dimming | `nos/apps/sys_faces.c:146-151`; `face_kit.h:141-146` |

Other data reaches the face's `face_ctx` but INVERTED never reads it: `bpm` (`svc_heart()->bpm_last`), `step_goal`, `weather_valid`, `temp_c`, `wx_cond`, `next_alarm_min` (`sys_home.c:477-487`). A grep of the file confirms it.

Helper reads: `nos_prefs.hour24` (`faces.c:79-90`); `hal_millis()` in `fk_wake_tick` (`faces.c:460`), used for wake and staleness; `nos_t` and the fade state of the shared unread dot (`faces.c:552-563`).

## Writes

No persistent write. No `prefs_*`, `svc_*` mutator, SD or NVS call.

- Own static state `S` (`face_inverted.c:38`): morph tracker, cached steps, wake state, restart and wake flags, `t0`, date cross-fade, and a sim-only probe.
- Shared unread fade: `static float s_unread_k` in `faces.c:548`, written by `fk_unread_tick` (`:552-559`). It is one value for every face.
- Shared dot-list cache: `fk_aod_update` bumps `s_dc_epoch` on every face tick (`faces.c:59`), which invalidates all face dot lists.
- Side effect on another face: while awake, `fk_aod_update` calls `face_segment_warm()` once the build-in is done (`faces.c:61`). That warms the SEGMENT face's scaled font in PSRAM, about 1 ms a frame until complete.
- AOD drift: `fk_aod_dx/dy` (`faces.c:62-63`).
- Strings: `fk_morph_tick` and `fk_xfade_tick` update their `from` and `to` buffers.
- Taps: only `nos_open(&scr_activity)` through the home screen (`sys_home.c:1126-1127`).

## Background behaviour

What keeps running, and who runs it:

- **The face itself** runs only while home is the top screen, or during a screen transition. `tick_screens` ticks only the top screen or the transition's under and over screens (`nos/nos.c:2101-2108`). With an app open, the face is neither ticked nor drawn.
- **Awake (PWR_ON):** the face ticks and draws at NOS_FPS = 40 Hz in the UI frame loop (`nos/render_config.h:8`, `nos/nos.c:2341`, `nos/nos.c:2510`). The frame loop runs in `app_main` (`firmware/main/nos_main.c:1899`). The `sdkconfig` pins the main task to CPU0 (`firmware/sdkconfig:1707`).
- **AOD (PWR_AOD):** one frame per second, from the `now - last_aod >= 1000000` block (`firmware/main/nos_main.c:2114-2118`). The face draws its AOD variant at that rate.
- **Screen off (PWR_OFF):** the face is not drawn. The loop sleeps 20 ms at 80 MHz and runs services once a second (`nos_main.c:2091-2094`).
- **Idle timeout:** `nos_prefs.sleep_s`, default 15 s (`nos/prefs.c:149`). The target is `power_idle_target` (`nos/power_policy.h:13-14`, `nos_main.c:2084-2085`). The default `aod = 0` (`prefs.c:144`) sends an idle B to OFF unless `sensor_idle` is set.
- **Services that feed the face keep running in every state.** `services_tick` (`nos/apps/registry.c:62-64`) runs `svc_tick` and `background_tick` from the frame loop, at 40 Hz awake, 1 Hz AOD and 1 Hz OFF. Inside `svc_tick`, `sys_tick`, `act_tick` and `sports_tick` run at 1 Hz (`nos/svc.c:1315-1319`), and heart history every 60 s (`:1322`).
- **Pedometer:** the `board-sense` task (`firmware/main/board.c:967-1025`, priority 2, 4 KB static stack, unpinned; created at `:1036`) reads the QMI8658 at about 20 ms per cycle (`:1023-1025`). It publishes `s_activity` (`:1007-1009`). `hal_activity_get` reports `valid = 0` if the last sample is over 250 ms old (`:495-505`).
- **Power sampling:** `hal_esp32_sample_power()` once a second from the same task (`board.c:1021`, `firmware/main/hal_esp32.c:316-392`).
- **Pedometer persistence:** the `board-fs` task (`board.c:919-955`, priority 1, created at `:1042`) saves the NVS key `pedometer` every 60 s and retries after 5 s.
- **Phone time:** `apply_time` from BLE sets the clock and time zone (`nos/svc.c:1000-1019`), so the face's time follows the phone.

## Persistence

- The face persists nothing. `enter()` resets only its morph tracker and restart flag (`face_inverted.c:40`). Its state is lost on face change or reboot.
- **Face selection** is saved by the picker. `finish()` writes `nos_prefs.face` and calls `prefs_save()` (`nos/apps/sys_faces.c:370-376`). The prefs are checkpointed to SD as `/watch/settings.<slot>.bin` (`nos/prefs_disk.c:32`), with an NVS mirror through `hal_store_save_checked("nosprefs")` (`firmware/main/hal_esp32.c:228-256`). They are loaded at boot (`:258-285`). `face` is the default 0, which is DOT (`nos/prefs.c:148`).
- **Activity steps** (the source of the number this face shows) are checkpointed to NVS key `nosact`. The checkpoint runs every 10 minutes when dirty, with a 5 s retry (`nos/svc.c:496-508`, `:557-558`). On B this key goes to NVS, not SD. `hal_store_save_checked` only routes `nosprefs` and `wifi_cred` to SD (`hal_esp32.c:236-252`).
- **Pedometer state** is saved to NVS key `pedometer` (`firmware/main/board.c:915`, `:955`).
- **Notifications** (the source of `unread`) are RAM only. The notification store has no `hal_store` call (`nos/svc.c:206-236`). The only persisted keys in `svc.c` are `nosact` and `nosalarm` (`:499`, `:565`, `:728`, `:735`). The unread count therefore resets to 0 on reboot.
- **Clock:** the PCF85063 RTC restores system time at boot (`firmware/main/board.c:871`). The face reads `gettimeofday` and `localtime_r` (`hal_esp32.c:116-130`).

## Navigation

- **Opened from:** home page 0 when `nos_prefs.face == 6` (`nos/apps/sys_home.c:271-275`). Chosen in the picker (`nos/apps/sys_faces.c:370-376`). The picker opens from a long press on home (`sys_home.c:1138-1140`) and from `home_open_faces()` (`sys_home.c:1010-1013`), which a face calls from its own gesture (the comment names ORRERY's 3 s hold).
- **Tap:** `face_inverted_hit` returns `&scr_activity` for x in (120, 346) and y in (310, 380) (`face_inverted.c:165-172`). The press highlight rect is 120, 318, 346, 356 (`:168`). Home dispatches taps through `f->tap` (`sys_home.c:1117-1127`). `scr_activity` is `nos/apps/fit_activity.c:547`. The highlight comes from `nos_face_press_rect` (`nos/faces/faces.c:830-838`, `sys_home.c:1063-1066`).
- **Other touches:** the date, time, status and unread dot have no hit test. The face is not tappable at those points.
- **Pager neighbours:** page 1 is the app launcher (`sys_home.c:903`). Pages 2 to 6 are the Activity, Heart, Weather, Music and Timer widgets (`sys_home.c:203`).
- **Style:** `FACE_LIGHT | FACE_FULL_BLEED` (`faces.c:816`). The page dots render dark and the press fill is #E0E0E0 (`nos/faces/face_ext.h:5-9`).
- **Wake:** the touch that wakes the watch is swallowed and never taps (`firmware/main/nos_main.c:2003-2006`).

Related: [[Activity]], [[Face picker]], [[Notifications]], [[Shell and faces]], [[Engine and scheduling]], [[Services hub]], [[Health and activity]], [[Power and battery]], [[Storage and persistence]], [[Hardware and drivers]], [[Connectivity and voice]].

Hardware it depends on (1.43-B, `firmware/main/pins.h:52-66`): CO5300 AMOLED over QSPI for the display; FT3168 touch at 0x38 for taps; QMI8658 IMU at 0x6B with interrupt 8, for steps; PCF85063 RTC at 0x51 with interrupt 15, for time across power-off; battery ADC on GPIO4 with divider 3. There is no charge-status pin on B. The `usb` flag comes from the USB-serial-JTAG connection (`hal_esp32.c:323`), see Gaps.

## Gaps

1. **[medium] Charging indicator on B is not a charge signal.** `usb_serial_jtag_is_connected()` sets `external_power = 1` (`firmware/main/hal_esp32.c:323`, `:350-351`). B has no `PIN_CHG_STAT` (`firmware/main/pins.h:52-66`; only the C block defines it, `:33`). The fallback is a rail above 4.4 V for 3 samples (`hal_esp32.c:347-349`). A PC connection therefore shows the bolt and percentage (`nos/faces/faces.c:587-600`), whether or not current is flowing.
2. **[medium] Battery shows stale values, and an empty cell shows nothing.** `hal_power_get` keeps the last `battery_percent` after 3 s without a sample and clears only the validity flags (`hal_esp32.c:378-390`). The face trusts any value above 0 (`faces.c:582-585`). `lipo_percent` returns 0 at or below 3.3 V (`hal_esp32.c:304`), and 0 is excluded from the status, so a dead cell gives no warning on the awake face or in AOD.
3. **[medium] Step count is not marked as unknown.** When the pedometer is invalid, `act_tick` publishes `s_as.max_steps` instead (`nos/svc.c:533`). That is the last NVS checkpoint, up to 10 minutes old (`:557-558`). The face shows it as a live count with no marker. Without an IMU (the C board, `pins.h:27`), the label reads "0 STEPS" rather than being hidden.
4. **[medium] No complications or per-face options.** Heart rate, weather, next alarm and step goal are all in `face_ctx` (`face.h:17-35`) and computed every frame (`sys_home.c:477-487`), but INVERTED draws none of them. The picker only chooses among the 9 fixed faces. Modern watch faces normally offer a configurable complication slot.
5. **[low] Unread dot is passive.** It shows presence only, with no count (`face_inverted.c:153`). It has no tap target (`:165-172`), so the user must page to the list. The state resets on reboot (see Persistence).
6. **[low] Step goal is not shown.** `step_goal` is built (`sys_home.c:472`) and not read. The steps line is a bare count, with no progress toward the goal that Activity shows.
7. **[low] Dead setting for this face.** `face_accent` ("0 red, 1 white", `nos/prefs.h:21`) is stored in `prefs.c:34` and read nowhere else under `nos/`. The seconds dot is always `C_RED` through `fk_red` (`face_inverted.c:149`, `face_kit.h:151-155`), so the setting cannot change this face.
8. **[low] Tap has one target, and it can fire before the label is visible.** Only the steps line is tappable (`face_inverted.c:165-172`). The label is hidden for the first 9 frames of the opening (`:50-52`, `:136-137`), but the hit zone is live. Tap has no other actions (no alarm, timer or notification shortcut).
9. **[low, inference only] Full-white AMOLED face.** The face fills the whole 466 x 466 panel white (`:118`). On AMOLED, white is the highest-current pixel state. There is no power measurement for this face in the repo, so the cost is unmeasured.
10. **[low] Header claims not verified on device.** The header says the wake flash is fixed and gives a fill cost of about 4 ns/px (`face_inverted.c:9-19`). `docs/polish/faces_r1.md:53-60` describes the pre-fix flash. The code now gates ghost cells on the opening fade (`:50-53`), which matches the claim, but no device capture in this pass confirms it.
11. **[low] Face only runs when home is on top.** While an app is open, the face is not ticked, so `unread`, `steps` and `enter` are not refreshed on the face itself (`nos/nos.c:2101-2108`). This is by design. On return, `sys_home_build_in()` restarts the build-in and calls the face's `enter` hook (`nos/apps/sys_home.c:278-284`).
