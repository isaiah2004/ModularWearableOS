---
title: "Compass"
id: app-compass
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Compass

Source: `nos/apps/out_compass.c` (734 lines). Sensor driver: `firmware/main/ext/ext_mag.c`. Read-only source review; nothing built, flashed or run, and no serial access. Evidence is `repo-relative/path:line`, shortened as follows: `ext_*.c` and `ext_priv.h` mean `firmware/main/ext/`; `out_*.c`, `set_*.c`, `registry.c` and `nos_apps.h` mean `nos/apps/`; `nos.c`, `nos.h`, `sports.c`, `background.c`, `diag_log.c`, `hal_ext.h`, `power_policy.h`, `motion.h` and `render_config.h` mean `nos/`; `nos_main.c`, `board.c` and `hal_esp32.c` mean `firmware/main/`. Behaviour below is traced from code, not bench-tested, unless stated.

## Summary

Compass is a launcher app (`scr_compass`, `nos/apps/out_compass.c:733`; launcher entry `nos/apps/registry.c:26`). It shows the heading from the GY-271 magnetometer on a 72-tick dial, walks the user through a figure-8 calibration, and has a bubble level driven by the QMI8658 accelerometer.

The screen calls the HAL directly: `hal_mag_get`, `hal_mag_calibrate`, `hal_imu_read` and `hal_ext_present`. It does not call any `svc_*` getter or any `prefs_*` function, and it reads no `nos_prefs` field itself. The magnetometer driver, not the app, owns the only persistent state: the calibration, stored in NVS key `ext_mag`. The driver runs in the single `ext` task, so the sensor is shared with other screens (see Navigation).

Points from the trace that matter most:

- The display never auto-sleeps while Compass is on top. It sets `SCR_KEEPAWAKE` (`out_compass.c:734`), which blocks the timeout sleep (`firmware/main/nos_main.c:2079-2084`).
- The heading is magnetic north. No declination code exists in `nos/`, `core/` or `firmware/main/`, so `docs/os-map/systems/Utility apps.md:62` ("with calibration and declination") is wrong.
- Several code paths degrade the result without telling the user: a partial calibration saved on leave, the "Not connected" page during speaker output, and a flat-watch assumption when IMU data is stale. See Gaps.

Status: partial. The dial, calibration, level and absent-module paths are all present in code. The gaps below are the reason it is not "working".

## Features

- **Dial** (`dial_draw` `out_compass.c:448-509`, `lubber_draw` `:511-518`). 72 tick dots (`N_TICK` 72, `R_TICK` 200). N, E, S and W in dot caps on the 90-degree marks, degree numerals every 30 degrees. The card rotates under a fixed red lubber mark at 12 o'clock.
- **Heading motion** (`tick_heading` `:232-272`). A critically damped spring on the unwrapped angle, capped at one tick pitch (`DIAL_CAP` 5 deg) per frame, settling in steps of at least `DIAL_MIN` 1 deg, with a `DIAL_DEAD` 0.6 deg deadband (`:52-56`). The displayed value is quantised to 0.5 deg (`:261-262`).
- **Readout** (`readout_draw` `:520-542`, `readout_set` `:217-230`). Integer degrees at `UI_CY-52`, morphing between values. An eight-point cardinal (`cardinal8` `:134-139`) sits under it.
- **Details pill** (tap on dial, `compass_input` `:711-721`; `readout_draw` `:529-538`). The cardinal fades out. The pill shows "HOLD FOR LEVEL", "FIELD nn uT" from `hal_mag.field_ut`, and a "Calibrate" button.
- **Calibrate** (`start_cal` `:177-188`, `cal_draw` `:562-589`). Prompt "Move your wrist in a figure 8" (`:585`), a 60-dot progress ring and a percentage. Coverage is 20 direction bins for 100 % (`ext_mag.c:142-146`). Manual recalibration from the details "Calibrate" button (`:714-716`) keeps the old calibration until progress restarts below 10 % (`compass_tick` `:383-386`).
- **Calibrated check** (`cal_done_draw` `:591-596`, mode `M_CAL_DONE` `:416-424`). A check mark and "CALIBRATED", then the dial. Hand-over after `M_FRAMES(33)`, about 0.8 s at `NOS_FPS` 40 (`nos/motion.h:26`, `nos/render_config.h:8`).
- **Level** (long press on dial or level, `compass_input` `:724-727`; `tick_level` `:274-319`; `level_draw` `:599-635`). Bubble on fixed rings at 6 px per degree, clamped to 112 px (`:285-287`). Readout "x.x°" read off the bubble, or "FLAT" with 1.0° in and 1.5° out hysteresis (`:310`). Accelerometer only.
- **Hold ring** (`hold_ring_draw` `:546-559`). A band of dots fills clockwise round the bezel during a press; the long press fires at 0.45 s (`LONG_S`, `:48`).
- **Absent page** (`face_draw` `:669-673`). "Not connected", "Add the compass to see your heading", footer "COMPASS MODULE". Shown when the `EXT_MAG` bit is clear at enter (`out_compass.c:203`).
- **Feedback** (`nos_feedback`): tick when level becomes flat (`:312`), success on calibration done (`:410`), tap on details (`:719`) and on the Calibrate button (`:715`).

## Reads

Service getters: none. `out_compass.c` includes `svc.h` (`:30`) and `prefs.h` (`:32`) but calls nothing from either.

Preferences: none read directly. `nos_feedback` (`nos/nos.c:557-573`) reads `nos_prefs.haptics` (`:570`) and `nos_prefs.sound` (`:572`) on every feedback event from this screen.

HAL and driver:

- `hal_ext_present()`: the `EXT_MAG` bit, read at enter (`out_compass.c:203`) and every tick (`:349`). The bit is set once in `mag_init` (`firmware/main/ext/ext_mag.c:101`). Nothing clears it (the only clears are for speaker and mic, `ext_audio.c:347`).
- `hal_mag_get(&C.mag)`: at enter (`out_compass.c:196`) and every tick (`:349`). Returns the cached `M->cur` (`ext_mag.c:217-229`). The result is invalid when no sample was computed in the last 500 ms (`ext_mag.c:226`). Fields: `valid`, `heading_deg`, `field_ut`, `calibrated`, `cal_progress` (`nos/hal_ext.h:191-197`).
- `hal_imu_read()`: level mode only (`out_compass.c:277`). Returns the cached QMI8658 sample, valid if younger than 250 ms (`firmware/main/board.c:388-396`).
- Engine: `nos_t`, `nos_enter_t` (`:206`), `nos_revealed` (`:193`), `nos_leaving_covered` (`:211`), `nos_ink_dim`, `nos_invalidate`, `nos_keep_awake` (`:413`). `nos_motion_probe` is a no-op unless `GFX_COST_MODEL` (`nos/nos.h:330-332`).

Not read: NVS, SD, `svc_*`, `prefs_*`. Calibration is read from NVS only by the driver, once, at boot (`ext_mag.c:97-98`).

## Writes

App (RAM only):

- `struct C` (`out_compass.c:70-102`): mode, a copy of `hal_mag`, the heading spring, the bubble spring, the calibration ring, hold state, plug-fade state. Cleared on a fresh enter (`:194`). Kept on a return from a covering screen (`:193`).
- `s_cal_trace` (`:176`), `S_OLD` (`:103`), `C.ro_cur`/`C.ro_prev` (readout strings).

Magnetometer driver, called by the app:

- `hal_mag_calibrate(1)` on start (`start_cal`, `out_compass.c:186`). Resets min/max and direction bins, sets `calibrating`, refreshes the idle timer (`ext_mag.c:231-242`, timer at `:234`).
- `hal_mag_calibrate(0)` on done (`:408`), on leave (`:212`) and on unplug mid-calibration (`:355`). It saves only when at least 10 of 26 bins are covered and every axis swings at least 10 uT (`ext_mag.c:246-252`). A save replaces `M->cal` and writes NVS (`:253-260`).

NVS: key `ext_mag` through `hal_store_save` (`ext_mag.c:258-260`). The return value is discarded (`firmware/main/hal_esp32.c:256-257`).

Diagnostics: `diag_action_request(DIAG_DOMAIN_SENSOR, DIAG_ACT_START, EXT_MAG)`, with context (name id "COMPASS", manual flag) and results (PENDING, SUCCESS, CANCELLED, FAILED) at `out_compass.c:179-187`, `:213`, `:356`, `:409`. The record holds state codes only.

Feedback and keep-awake: `nos_feedback` at `:312`, `:410`, `:715`, `:719`. `nos_keep_awake` at `:413` while calibrating.

Sensor pushes (triggered by the app's reads, not written by the app): `sports_emit(SP_MAG_RAW)` for each successful read (`ext_mag.c:210`) and `sports_emit(SP_MAG)` for each computed heading (`ext_mag.c:187`). `SP_MAG` feeds the COMPASS background job (`nos/background.c:99`, `:112`). Both feed the SD sports journal (see Persistence).

## Background behaviour

| Who | What it does | Period, priority, core | Evidence |
|---|---|---|---|
| `ext` task | Calls `mag_service()` every loop. Sleeps for up to 50 ms, or for whatever the shortest module request is. | Priority 0. 4096 B stack in PSRAM. `tskNO_AFFINITY` (unpinned). | `firmware/main/ext/ext_sys.c:144-167`, `:186-187` |
| `mag_service` | Standby when no consumer is active. Reads and computes when running. | Idle returns 1000 ms (`ext_mag.c:201`). Running reads every 50 ms, which is 20 Hz (`:203`). | `ext_mag.c:191-215` |
| Chip | QMC5883L runs continuously at 50 Hz, ±8 G, 30 LSB per uT. HMC5883L runs at 15 Hz. Standby is a mode-register write. | The chip rate is not the read rate. | `ext_mag.c:4-5`, `:70-74` |
| Run condition | Running while calibrating, or within `NOS_EXT_IDLE_MS` (8000 ms) of the last `hal_mag_get`/`hal_mag_calibrate`. | The sensor stops about 8 s after the last call. | `ext_mag.c:195`; `nos/hal_ext.h:35`; `firmware/main/ext/ext_priv.h:59` |
| Skip rule | Samples are dropped (50 ms skip, no compute) while the vibration motor is busy or the amp is on. | Haptic busy window is segment length + 40 ms (`ext_haptic.c:78`). Amp tail 60 ms (`ext_audio.c:39`). | `ext_mag.c:204` |
| Screen on, top of stack | `compass_tick` runs every frame and calls `hal_mag_get`. Drawing happens only on damage. | Engine frame clock. | `nos/nos.c:2101-2110`; `out_compass.c:349` |
| Screen on, keep-awake | Timeout sleep and AOD are blocked for as long as Compass is on top. | Indefinite while open. | `out_compass.c:734`; `nos/power_policy.h:18-19`; `firmware/main/nos_main.c:2079-2084`, AOD check `:2116-2121` |
| Screen off | The frame loop is skipped. Only the 1 Hz background tick runs. `compass_tick` does not run, so the sensor is no longer polled and stops about 8 s later. | 1 Hz `nos_bg_tick`. | `firmware/main/nos_main.c:2091-2094`; `ext_mag.c:195` |
| Closed (leave) | `compass_leave` stops a calibration only when not covered. Otherwise the sensor idles after 8 s. | Leave-time. | `out_compass.c:209-215`; `nos/nos.h:179-181` |
| Covered during calibration | `calibrating` overrides idle, so the sensor keeps running. The covered screen does not tick outside transitions, so calibration does not progress or time out. | Until the screen is revealed and finishes, or leaves with `nos_leaving_covered()==0`. | `ext_mag.c:195`; `nos/nos.c:2101-2110`; `nos/nos.h:179-181` |
| Other consumers | Navigate, the Modules detail page and the Diagnostics MAG test each call `hal_mag_get` while open, which keeps the sensor up for them. | While those screens are open. | `nos/apps/out_nav.c:662`; `nos/apps/set_modules.c:221`; `nos/apps/set_diag.c:368` |
| Sports sampler | Polls the magnetometer in the simulator only. On device the driver pushes samples at acquisition. | n/a | `nos/sports.c:394-401` (inside `#ifndef ESP_PLATFORM`) |
| COMPASS background job | Passive. Records `SP_MAG` pushes, `on_demand`, stale after 3000 ms. It never polls the sensor. | Job tick 1 Hz. | `nos/background.c:34`, `:99`, `:160-186`, `:175-178` |

## Persistence

- **NVS**, namespace `wos`, key `ext_mag` (`firmware/main/hal_esp32.c:236`; `nvs_save` `:177-187`). The blob is the driver's `mag_cal` struct: magic `MAG1`, hard-iron offsets, soft-iron scales, axis map, forward axis, valid flag (`ext_mag.c:37-43`). Written after a successful calibration (`:258-260`) and by the serial command `ext mag map`, which uses the checked save (`:294`). Loaded once at boot (`:97-98`). Not mirrored to SD: only `nosprefs` and `wifi_cred` take the SD path (`hal_esp32.c:228-257`).
- **SD, sports journal.** Every `SP_MAG` and `SP_MAG_RAW` record is a raw type (`nos/sports.c:56`, `:134-140`) and is appended to `/sdcard/watch/sports/<YYYYMMDD>-<HH>-sensors.spt` (root `nos/sports.c:25`, path `:215-219`, drain `:243-262`) by the `sports_sd` writer (priority 1, `:282-292`, creation `:323`). The writer drains at least once a second. The journal is not gated by the Compass screen or by any setting. It runs whenever the sensor runs, from any consumer.
- **SD, diagnostics.** Calibration action records go to the RAM diagnostics ring, drained to `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`). They carry state codes and the "COMPASS" name id, never readings (`out_compass.c:179-181`).
- **RAM only.** The app's `struct C`, the dial and level state, and the COMPASS job history (allocated with `nos_mem_alloc`, `nos/background.c:120`). All reset on reboot. The heading and level are never saved.
- No `prefs` key, no svc entry, no Gadgetbridge message.

## Navigation

Opened from:

- Launcher app list, "Compass" with `DOT_ICON_COMPASS` (`nos/apps/registry.c:26`). The only other reference to `scr_compass` is its extern in `nos/apps/nos_apps.h:51`. No Home widget, face complication or other screen opens it.

Opens: nothing. The app never calls `nos_open` or `nos_push`.

Modes (`out_compass.c:66`): `M_ABSENT`, `M_CAL`, `M_CAL_DONE`, `M_DIAL`, `M_LEVEL`. Presence and the calibrated flag pick the entry mode (`compass_enter` `:191-207`).

Input (`compass_input` `:696-731`):

- Tap on dial: toggles the details pill (`:718`). Tap on the details "Calibrate" button: manual recalibration (`:714-716`).
- Press and hold on dial or level: the hold ring fills. A long press swaps dial and level (`:701-704`, `:724-727`).
- Tap on level, on the calibration page or on the absent page: nothing (`:722-723`).
- Cancel clears the press state (`:698`).
- The calibration page has no button. Back (engine gesture) is the only exit, and it runs `compass_leave`. `scr_compass` does not set `SCR_NOBACK` (`:733-734`).

Related screens that share the sensor:

- Navigate: "BY COMPASS" heading source when calibrated (`nos/apps/out_nav.c:662`; label `:1005`).
- Modules, Compass row: presence and reading (`nos/apps/set_modules.c:81-82`, `:221`, `:323-325`), with its own CALIBRATE and DONE (`:1381-1392`) and detail leave (`:958-960`).
- Diagnostics, MAG test: plausibility check of 20-70 uT (`nos/apps/set_diag.c:75`, `:368-380`).

## Gaps

1. **[high] The display never auto-sleeps while Compass is open, and the sensor is polled every frame.**
   Evidence: `out_compass.c:734` (SCR_KEEPAWAKE); `firmware/main/nos_main.c:2079-2084` (keep blocks the timeout); `nos/power_policy.h:18-19`; the same flag wakes the panel from OFF and AOD (`nos_main.c:2095-2096`, `:2121`); `out_compass.c:349` (`hal_mag_get` every tick).
   Detail: the header says the screen "costs nothing" once settled (`out_compass.c:6-7`). That is true for drawing only. The frame tick, sensor polling and keep-awake continue. There is no timeout, no wrist-down pause and no user setting.
   Impact: a Compass left open on a table or in a pocket holds the screen and the magnetometer until the user leaves the screen. This is a battery cost. It has not been measured here.

2. **[medium] Speaker output blanks the dial to "Not connected" while the module is present.**
   Evidence: `out_compass.c:349-352` (real = valid and present); `ext_mag.c:204` (samples skipped while `audio_amp_on()`); `ext_mag.c:226` (valid expires after 500 ms); `out_compass.c:352-363` (flip to `M_ABSENT`); `out_compass.c:669-673` (absent text). `ext_audio.c:286-288` sets the amp on for any PCM or SFX output when volume is above zero, with a 60 ms tail (`:39`).
   Detail: haptics alone do not reach 500 ms. One haptic segment keeps the busy window at most 340 ms (`ext_haptic.c:78`, longest segment `:31`). The "NO COMPASS DATA" branch (`out_compass.c:677-680`) is drawn only for the first 25 ms of the 58 ms fade (`plug_phase` `:339-345`; `PLUG_A`/`PLUG_B` `:324-325`). The user therefore sees "Not connected / Add the compass to see your heading" instead.
   Impact: the heading disappears during longer speaker output and returns with a fade afterwards, while the text says the module is missing. Not bench-run. `CLAUDE.md` lists the B strap's external audio mapping as needing its own verification, so whether this path fires on a real B strap is unverified.

3. **[medium] Leaving a calibration, or a manual recalibration, silently saves a partial result.**
   Evidence: `out_compass.c:209-215` (leave calls `hal_mag_calibrate(0)` and records CANCELLED at `:213`); `ext_mag.c:246-260` (saves at 10 or more of 26 bins and 10 uT or more per axis, replacing the stored calibration). The Modules detail page does the same (`set_modules.c:958-960`).
   Detail: the calibration page has no cancel control (`out_compass.c:696-731`). Back is the only way out, and the diagnostic then says CANCELLED even when a new calibration was written.
   Impact: backing out at around half coverage can replace a good calibration with a weaker one.

4. **[medium] Heading assumes a flat watch when the IMU sample is stale or weak, with no indication.**
   Evidence: `ext_mag.c:157-162` (gravity defaults to flat unless `hal_imu_read` succeeds and |a| is above 0.3 g); `firmware/main/board.c:388-396` (IMU sample valid for 250 ms).
   Detail: tilt compensation is dropped without a flag on screen.
   Impact: heading error when the wrist is tilted and the IMU sample stream stalls.

5. **[medium] No magnetic declination. The heading is magnetic north.**
   Evidence: `nos/hal_ext.h:193` ("0 = magnetic north"); `ext_mag.c:177-178` (atan2 only). A grep of `nos/`, `core/` and `firmware/main/` finds no declination code. `docs/os-map/systems/Utility apps.md:62` says "with calibration and declination", which is incorrect.
   Detail: the strap has a GPS module (`nos/hal_ext.h:122-139`), but Compass does not use it for location.
   Impact: bearings are off by the local magnetic declination, which can be many degrees.

6. **[medium] Raw magnetometer samples are journaled to SD while the sensor runs, with no user control.**
   Evidence: `ext_mag.c:187` (SP_MAG per computed heading); `ext_mag.c:210` (SP_MAG_RAW per read); `nos/sports.c:56` (both are raw types); `:134-140` (non-essential records are queued unless quiesced); `:215-219`, `:243-262` (appended to the hourly `*-sensors.spt` file).
   Detail: up to 20 SP_MAG and 20 SP_MAG_RAW records per second while the sensor runs. Navigate, Modules and Diagnostics trigger it too, not only Compass. Rate and SD energy not measured here.
   Impact: SD writes and power on every compass use, with no toggle and no note in the app.

7. **[medium] No interference or field-strength warning on the dial.**
   Evidence: field is shown only as "FIELD nn uT" in the details pill (`out_compass.c:534`). The driver notes that the strap pod sits near a speaker magnet and a motor (`ext_mag.c:6-11`). Diagnostics flags 20-70 uT as TOO WEAK or TOO STRONG (`nos/apps/set_diag.c:379-380`). Compass has no equivalent check.
   Impact: a wrong heading near metal or a speaker looks the same as a right one.

8. **[low] The first open after boot replays calibration, even when a stored calibration exists.**
   Evidence: `out_compass.c:196-205` (enter decides from `hal_mag_get` before any sample exists; `M->cur` stays zeroed until the first compute, `ext_mag.c:185-186`, `:217-222`); `out_compass.c:387` and `:407-412` (done as soon as `calibrated` is set); `ext_mag.c:246-252` (no save, previous calibration kept).
   Detail: the calibration ring, success feedback and a SUCCESS diagnostic all appear, though nothing new was learned. "CALIBRATED" is shown whatever the save outcome. Code-traced only. `docs/os-map/systems/Utility apps.md:184` already flags it as unverified.
   Impact: a false calibration prompt on the first open after each boot.

9. **[low] The magnetometer runs in Level mode, which uses only the accelerometer.**
   Evidence: `out_compass.c:349` (`hal_mag_get` runs in every mode); `:429-432` (level mode runs only `tick_level`, which reads `hal_imu_read` at `:277`).
   Impact: 20 Hz reads and compute while the level result never uses them. A small power cost.

10. **[low] The calibration save result is not checked.**
    Evidence: `ext_mag.c:258-260` uses `hal_store_save`, and `firmware/main/hal_esp32.c:256-257` discards the result. RAM already holds the new values (`ext_mag.c:259`).
    Impact: if the NVS write fails, the new calibration works until reboot and then silently reverts. The serial `ext mag map` path uses the checked save (`ext_mag.c:294`).

11. **[low] A calibration left covered keeps the sensor running.**
    Evidence: `ext_mag.c:195` (`calibrating` overrides idle); `nos/nos.h:179-181` (a covered screen keeps its state); `nos/nos.c:2101-2110` (screens outside transitions do not tick).
    Impact: the chip, compute and SP_MAG journal writes continue while another screen is on top, until Compass is revealed and leaves normally.

12. **[low] Presence is latched at boot.**
    Evidence: `ext_mag.c:81-101` (probe only in `mag_init`); `firmware/main/ext/ext_sys.c:182` (called once from `hal_ext_init`); `ext_sys.c:197-200` (bits are returned as-is; nothing clears the MAG bit, see Reads).
    Impact: a strap that fails at boot shows "Not connected" until reboot. A strap unplugged later shows "Not connected" only after the 500 ms valid timeout, because the presence bit never clears.

13. **[low] Axis map and mounting cannot be set on the watch.**
    Evidence: `ext_mag.c:281-298` (`ext mag map` is serial only); defaults at `ext_mag.c:61-68` (identity map, forward +X); header `:13-19`.
    Impact: if the strap pod is mounted differently, a USB serial session is needed to fix the heading.

14. **[low] Level is accelerometer-only and has no motion gate.**
    Evidence: `out_compass.c:277-283` (raw accel each frame); `:310` (FLAT hysteresis).
    Impact: the bubble and FLAT read tilt plus linear acceleration while the wrist moves. It is not a gyro-fused level.

15. **[low] Feature gaps for a modern compass.**
    Evidence: no bearing lock, saved bearing or back-to-start state in `out_compass.c`. No Home widget or face complication (`scr_compass` appears only at `registry.c:26`). Navigate has a GPS home bearing (`out_nav.c:662`, `:1005`); Compass has no equivalent.
    Impact: this app cannot hold a heading, point to a saved place, or show true north. Inclination is limited to the level screen.

16. **[low] Docs overstate or lag the code.**
    Evidence: `docs/os-map/systems/Utility apps.md:62` (declination, incorrect); `:64` ("runs only while open" misses the 8 s tail and the other consumers); `out_compass.c:6-7` ("costs nothing" once settled).
    Impact: a misleading picture for the next reader. The Utility apps note should be corrected in the same pass as this one.

## Hardware

- Magnetometer: QMC5883L at I2C `0x0D`, or HMC5883L at `0x1E`, on the strap module (`nos/hal_ext.h:16`; probes at `firmware/main/ext/ext_mag.c:81-88`). The strap shares the I2C bus on SDA 47 and SCL 48 (`firmware/main/ext/ext_priv.h:66`; `nos/apps/set_modules.c:82`).
- Accelerometer: QMI8658 at `0x6B` on the base board (`CLAUDE.md`, hardware table), read through `board_imu_read`.
- SD card: needed only for the sports journal and diagnostics. The calibration works with no SD card, because it lives in NVS.
- Vibration motor and speaker amp: they are not required, but when active they cause sample skips (see Background).

## Related notes

[[Services hub]] (no svc getter is used here), [[Hardware and drivers]] (the magnetometer driver, `ext` task), [[Engine and scheduling]] (the frame tick and the `ext` task loop), [[Power and battery]] (keep-awake, idle stop, screen-off path), [[Storage and persistence]] (NVS `ext_mag`, SD journal and diagnostics), [[Sports and workouts]] (the SP_MAG journal), [[Jobs]] (the COMPASS background job), [[Shell and faces]] (launcher and screen stack), [[Utility apps]] (the group this app belongs to).
