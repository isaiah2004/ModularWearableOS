---
title: "Power / screen-off screen"
id: sys-power
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Power / screen-off screen

Source: `nos/apps/sys_power.c` (371 lines), screen `scr_power`, title "POWER". Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from source reading only.

## Summary

- The long-press power sheet. A side-button hold of 600 ms (`nos/nos.c:592`) pushes `scr_power` as a `TX_RISE` sheet over whatever is on screen (`nos/nos.c:953-955`). The only registration is `nos_power_screen = &scr_power` (`nos/apps/registry.c:72`).
- Shows the battery as a dot-matrix percentage over ten cells, under the title BATTERY or CHARGING (`nos/apps/sys_power.c:290-298`).
- Three actions. SLEEP turns the panel off at once. RESTART and POWER OFF arm on the first tap and commit on a second tap within 3 s. Commit calls `nos_host_power(1|2)` after 0.8 s (`sys_power.c:182-186`).
- Device side of that call (`firmware/main/nos_main.c:751-762`): a shutdown guard, a preference flush and a board park, then `esp_restart()` (mode 1) or GPIO0 deep sleep (mode 2) (`nos_main.c:763-811`). SLEEP is `go_state(PWR_OFF)` only (`nos_main.c:754`), which is not a deep sleep.
- Owns no task, timer, ISR, NVS key or SD file. All of its state is RAM (`sys_power.c:47`, `:70-85`).
- Its battery number is a 1 Hz copy of the sampler's `battery_percent`. It never reads `battery_valid`, so a placeholder or a stale number is shown as fact (Gaps 1 and 2).
- Hardware it needs: the panel and FT3168 touch through the engine, the BOOT/GPIO0 side button (`firmware/main/nos_main.c:479-488`; the deep-sleep wake at `:810`), the battery ADC on GPIO4 with divider 3 (`firmware/main/pins.h:65-66`), and the USB Serial/JTAG host detect used as "charging" (`firmware/main/hal_esp32.c:323`). Haptic and tone are optional through `nos_feedback`.
- Related: [[Power and battery]] (sampler, shutdown path), [[Engine and scheduling]] (tick, keep-awake, cover), [[Shell and faces]] (launcher, home, back gesture), [[Storage and persistence]] (prefs checkpoint), [[Hardware and drivers]] (panel, touch, ADC, BOOT), [[Services hub]] (the `svc_sys` mirror), [[Connectivity and voice]] (BLE stays up; Chat refuses restart and off), [[Sports and workouts]] (journal quiesce, recording guard), [[Health and activity]] (not read here).

## Features

1. **Battery hero.** Title `CHARGING` when `svc_sys()->charging`, else `BATTERY` (`sys_power.c:290`). The percentage is `"%d%%"` in the dot font, revealed over 0.40 s (`:291-293`).
2. **Ten-cell level.** `cells = round(pct / 10)`, at least 1 when pct is above 0 (`:171`). The field comment at `:84` says "at least 1", but at 0 % the code lights 0 cells (`:171`). The cells sweep in over `14 + cells` frames (`:190-198`) and fill to the level (`:297-298`).
3. **SLEEP.** One tap, no arming. It clears any arm, plays FB_TAP, pops to home (`:358`) and calls `nos_host_power(0)` (`:359`). That becomes `go_state(PWR_OFF)`: panel brightness 0, panel SLPIN/DISPOFF, CPU capped at 80 MHz (`firmware/main/nos_main.c:697`, `:2092`). BLE, services and the journal keep running.
4. **RESTART and POWER OFF.** The first tap arms (`arm()`, `sys_power.c:123-133`). The pill grows to 320 px in mechanical cuts, with the copy "TAP AGAIN TO RESTART" or "TAP AGAIN TO POWER OFF" in red (`:229-230`, `:278`). Three 14 x 5 segments drain over 3 s (`:281-283`). A second tap within 3 s commits (`:135-142`, `:360-362`), and the label changes to "[ RESTARTING ]" or "[ POWERING OFF ]" (`:231`). The arm auto-clears when the last dot goes out (`:172`).
5. **Feedback.** Arm gives FB_LIMIT (`:132`). Restart commit gives FB_TAP and power-off commit gives FB_ERROR (`:141`). SLEEP gives FB_TAP (`:357`). The haptic and sound map is at `nos/nos.c:557-568`.
6. **Press feedback.** The pressed fill shows after 90 ms if the finger is within 3 px. It cancels when travel passes 8 px (`sys_power.c:45-68`).
7. **Cancel paths.** A tap on empty space or another pill disarms (`:363`). A downward drag returns 0 so the engine closes the sheet (`:348`). A side-button press pops to home (`nos/nos.c:952`; `p_input` returns 0 for IN_BTN, `:366-367`). A side-button press during the shutdown window cancels the shutdown (`firmware/main/nos_main.c:2025`).
8. **Taps during a commit are ignored.** `IN_TAP` with `S.done >= 0` only cancels the press (`sys_power.c:351`).

## Reads

**Services**
- `svc_sys()` returns `svc_sys_t` (`nos/svc.h:179-185`; `nos/svc.c:986`). Fields read: `battery_pct` (`sys_power.c:168`) and `charging` (`:169`). Not read: `battery_valid`, `rail_mv`, `phone_connected`, `wifi_up`, `steps_today`.
- The snapshot is refreshed by `sys_tick()` (`nos/svc.c:987-996`), which copies `hal_power_get()` once per second from `svc_tick` (`nos/svc.c:1315-1317`).

**Prefs keys**
- None. `prefs.h` is included (`sys_power.c:21`), but no `prefs_*` or `nos_prefs` identifier appears in the file.

**HAL**
- `hal_battery_percent()`, used only when `battery_pct` is 0 (`sys_power.c:168`; `core/hal.h:32`; `firmware/main/hal_esp32.c:392`). It reads the same cached snapshot, so it is not a second source.
- `hal_power_get()` is reached only through the svc mirror.

**Engine and UI (no data)**
- `nos_t`, `nos_enter_t()` (`sys_power.c:154`), `nos_touch_travel()` (`:60`, `:67`), `nos_feedback()` (`:132`, `:141`, `:357`), `nos_home()` (`:185`, `:358`), `nos_host_power()` (extern at `:27`; device `firmware/main/nos_main.c:751`; sim `sim/nos_sim.c:95`).
- `nos_motion_probe()` (`sys_power.c:308-322`) is a no-op macro on device (`nos/nos.h:332`). It is live only in the simulator (`sim/nos_motion.c:105`).

## Writes

- **RAM only.** `static P` for press state (`sys_power.c:47-68`) and `static S` (`:70-85`). `p_tick` rewrites `S` every frame (`:164-188`), and `p_enter` resets both (`:144-155`). The sheet changes nothing else.
- **Host call, mode 1 or 2** (`firmware/main/nos_main.c:751-762`). Order: `shutdown_guard()` (`:731-750`), then `sports_quiesce(1)` (`nos/sports.c:132`), then `DIAG_POWER_POLICY 0x100` (`nos_main.c:759`), then `go_state(PWR_ON)`, `nos_keep_awake()`, `prefs_save()` (`:760`). `prefs_save()` only marks prefs dirty (`nos/prefs.c:260-265`). Then `hal_asset_cancel()` and the toast "WAITING FOR SHUTDOWN" (`nos_main.c:760-761`).
- **Host call, mode 0** (`nos_main.c:754`). `go_state(PWR_OFF)` writes `DIAG_POWER_STATE` (`:695`) and an `SP_DISPLAY_OFF` journal record through `sports_power` (`:706`; `nos/sports.c:192-195`).
- **Shutdown, mode 1 or 2** (`shutdown_tick`, `nos_main.c:763-811`). It runs `prefs_flush()` (`:778`, `:796`), `hal_ext_shutdown_begin/poll`, `board_prepare_sleep()` (`:797`; `firmware/main/board.c:1126`), and the panel off (`:806`). It then logs `DIAG_POWER_POLICY 0x101` (`:807`) and calls `esp_restart()` (`:809`) or `esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0)` and `esp_deep_sleep_start()` (`:810-811`).
- The sheet writes no SD file, NVS key or prefs field.

## Background behaviour

- **The sheet runs no background work.** `tick_screens` ticks only the top screen, or the under and over screens during a transition (`nos/nos.c:2101-2110`). A covered or closed sheet gets no `p_tick`.
- **Battery data is produced without it.** `hal_esp32_sample_power()` runs once per second inside `task-board-sense` (`firmware/main/board.c:970`, `:1021`; sampler `firmware/main/hal_esp32.c:316-388`). It runs with the screen off.
- **Sheet data refresh.** `sys_tick` runs at 1 Hz from `svc_tick` (`nos/svc.c:1315-1317`). `nos_bg_tick` calls `svc_tick` every ON frame (`nos/apps/registry.c:62-66`) and once per second in OFF (`firmware/main/nos_main.c:2094`).
- **OFF loop.** `cpu_mode(80)`, a 20 ms delay, no frames (`nos_main.c:2091-2101`). The sheet cannot be opened from OFF, because the waking press is swallowed (`:2067`).
- **Not keep-awake.** `scr_power` has flags 0 (`sys_power.c:371`) and never calls `nos_keep_awake()`. The sleep policy checks only `SCR_KEEPAWAKE` (`nos_main.c:2080-2084`; `nos/power_policy.h:18-19`). With the shipped defaults (`sleep_s=15` at `nos/prefs.c:149`, `sensor_idle=1` at `:159`), 15 s without a touch leads to `PWR_SENSOR`. `go_state` then calls `nos_home_now()` (`nos_main.c:705`), which dismisses the sheet. Entering OFF does not collapse the stack (`:697`), so the sheet is still there after a wake.
- **Shutdown continues after the sheet closes.** The commit calls `nos_home()` right after `nos_host_power()` (`sys_power.c:184-185`). The rest of the sequence then runs in the main loop (`shutdown_tick`, `nos_main.c:763-811`). Each pass calls `nos_keep_awake()` (`:766`). A side-button press cancels it (`:2025`).
- **SLEEP changes only the panel.** `go_state(PWR_OFF)` changes the panel, brightness and CPU cap (`nos_main.c:695-697`, `:2092`). BLE, services, the SD journal and the 1 Hz sampler keep running. See [[Power and battery]].

## Persistence

- **The sheet persists nothing.** Its state is rebuilt on every entry (`sys_power.c:144-155`).
- **Preferences.** Restart and power off mark prefs dirty (`nos_main.c:760`). The shutdown path flushes them (`:778`, `:796`) through `prefs_flush()` into `hal_store_save_checked("nosprefs")` (`nos/prefs.c:331-349`). That writes `/sdcard/watch/settings.0.bin` and `settings.1.bin` (`nos/prefs_disk.c:32`, reached from `firmware/main/hal_esp32.c:243-247`), plus the NVS mirror `nosprefs` (`hal_esp32.c:250`). SLEEP does not call `prefs_save()`. Its dirty prefs are flushed later by `prefs_tick()` in the OFF loop (`nos_main.c:2098`). See [[Storage and persistence]].
- **Journal.** Each state change emits `SP_POWER` (`nos_main.c:706`; `nos/sports.c:192-195`). `sports_quiesce(1)` pauses the journal before a shutdown (`nos/sports.c:132`).
- **Diagnostics.** `DIAG_POWER_STATE` (`nos_main.c:695`), `DIAG_POWER_POLICY` 0x100 (`:759`) and 0x101 (`:807`).
- **Not persisted.** Battery percent and charging. After a reboot the percent starts at the placeholder 50 (`firmware/main/hal_esp32.c:287`).

## Navigation

- **Opened by** the side-button long press. The dispatch is at `nos/nos.c:977-979`, and the push is at `:953-955` (`TX_RISE`). It opens only when the top screen does not consume `IN_BTN_LONG`. The launcher page (`nos/apps/sys_launcher.c:1013`) and the home face (`nos/apps/sys_home.c:1027`) pass it on. The boot screen (`nos/apps/sys_boot.c:205`) and the Diagnostics Button card (`nos/apps/set_diag.c:742-750`) consume it.
- **Also by** the serial command `open power` (`firmware/main/nos_main.c:930`, `:1558`) and the simulator screen list (`sim/nos_sim.c:73`).
- **Not opened from** Control Center or Settings. No other file references `scr_power`. Settings has its own Restart arm (`nos/apps/set_settings.c:486`, `:1825-1826`), which calls `nos_host_power(1)` at `:686` and bypasses this sheet.
- **Opens** nothing. It only calls the host.
- **Leaves to** the root with `nos_home()`: SLEEP at `sys_power.c:358`, restart and off after 0.8 s at `:185`, and a side-button press at `nos/nos.c:952`. A downward drag closes it through the engine (`sys_power.c:348`). The AOD and SENSOR timeout also dismisses it (`nos_main.c:705`).
- **Covers** whatever is underneath, and that screen receives `call_cover` and `leave` (`nos/nos.c:953-956`). Two other notes record the side effects. [[Incoming call screen]] shows that a ringing call is dropped with no REJECT by SLEEP, RESTART or OFF (`sys_power.c:355-358`; `nos/svc.c:975-979`). [[Find phone]] shows that the ring stops.

## Gaps

1. **[high] The hero shows `battery_pct` without its validity flag.** `sys_power.c:167-170` reads only `battery_pct` and `charging`. `battery_valid` is copied into the mirror (`nos/svc.c:994`) and then ignored. Settings > Battery shows "--" and hides the cells when the reading is invalid (`nos/apps/set_battery.c:46-47`). The initial value is 50 (`firmware/main/hal_esp32.c:287`). On battery, the percent changes only inside the valid branch (`hal_esp32.c:359-361`), and nothing resets it when validity is lost or the cable is pulled. `hal_power_get` clears only the flags (`hal_esp32.c:385-388`). Impact: on battery a false "50%" shows for about 10 s after boot, which is the stable-sample requirement. After unplugging, the last USB-era number is shown as current.
2. **[medium] No ADC-calibration fallback. On battery the number can stay at 50.** If `adc_cali_create_scheme_curve_fitting` fails, `s_cali` is set to NULL (`firmware/main/hal_esp32.c:2344-2345`). The valid branch and the high-rail branch both require `s_cali` (`:347`, `:355`), so `battery_valid` is never set and the on-battery percent stays at the placeholder. The sheet then shows "50%" and "BATTERY" with no sign of failure. This applies only if the scheme fails; it was not observed on hardware here. The USB branch (`:350-354`) does not need `s_cali`.
3. **[medium] "CHARGING" means USB host present or rail at or above 4.4 V.** `external_power=1` comes from host SOF (`hal_esp32.c:323`) or from three samples at or above 4400 mV (`:347-352`). B has no charge-status pin: `PIN_CHG_STAT` is defined only in the 1.43C block (`firmware/main/pins.h:33`). The sheet therefore cannot say full, trickle or not charging. A charger without data lines below 4.4 V reads as BATTERY. While on USB, the percent updates only when the smoothed rail is 3300-4300 mV (`hal_esp32.c:354`), so a full cell can keep an older number.
4. **[medium] No time-remaining or drain estimate.** `p_draw` uses only `S.pct`, `S.cells` and `S.charging` (`sys_power.c:288-305`). There is no runtime estimate and no time-to-full. Battery History, which has a 24-bin level and mode stripe (`nos/apps/fit_energy.c`), is not linked from the sheet. The user gets a bare percentage and no sense of runway.
5. **[medium] No battery-saver control or status on the sheet.** The low-battery display saver starts automatically at 15 % or below and clears at 20 % or above (`nos/power_policy.h:6-11`; `nos_main.c:2031-2037`). The sheet does not show whether it is active. The `sensor_idle` mode, which is the default state after the idle timeout, can be changed only from Battery History (`nos/apps/fit_energy.c:155`, `:247`). The user cannot pick a saver mode from the power menu.
6. **[medium] Refused or failed power actions are reported only by a toast, and some messages are wrong.** The sheet closes whatever the result (`sys_power.c:184-185`). `nos_host_power` returns after a toast (`nos_main.c:755-757`). A voice chat is cancelled by the guard and then reported as "SAVE RECORDING FIRST" (`:734`, `:757`). SD dirty, SD uncertain and track-pending all return the same code and text (`:744-746`, `:757`). A second request while a shutdown is pending is ignored silently (`:755`). Impact: the user is told to save a recording when the real cause is a voice chat or SD state, and a repeated tap does nothing. See [[Chat]] and [[Sports]].
7. **[medium] A ringing call is dropped by SLEEP, RESTART and OFF.** `sys_power.c:355-358` pops the stack, and `call_find_tick` then sets the call state to 0 with no REJECT (`nos/svc.c:975-979`). Impact: the phone keeps ringing, the watch sends nothing, and the user is not told. Documented in [[Incoming call screen]].
8. **[medium] POWER OFF cannot be woken by a timer or alarm, and the sheet says nothing.** The only deep-sleep wake is GPIO0 (`nos_main.c:810`). The sheet shows no warning before power off. Impact: an alarm or timer set before POWER OFF will not sound. See [[Power and battery]] gap 10 and [[Timer]].
9. **[low] SLEEP is display-off, not sleep.** The label suggests a low-power state. `go_state(PWR_OFF)` changes only the panel, brightness and CPU cap (`nos_main.c:695-697`, `:2092`), and background work continues (`:2091-2101`). Impact: a user who presses SLEEP to save power still pays for background work.
10. **[low] The sheet is not kept awake and closes itself after the sleep timeout.** `sys_power.c:371` has flags 0, and the policy checks only `SCR_KEEPAWAKE` (`nos_main.c:2080-2084`). With `sleep_s=15` (`nos/prefs.c:149`) and `sensor_idle=1` (`:159`), the sheet disappears after 15 s without a touch (`nos_main.c:705`). If the timeout goes to OFF instead, the sheet remains. Users reading the number may be surprised by the dismissal.
11. **[low] On a locked watch the sheet opens but none of its buttons work.** A side long press opens it (`nos/nos.c:953-955`, no lock check). A new touch is dropped with only the unlock hint (`nos/nos.c:665-666`). The side button closes the sheet (`:952`). Impact: POWER OFF and RESTART cannot be used on a locked watch, and the sheet is read-only.
12. **[low] No progress feedback after commit.** The sheet closes at `sys_power.c:185`, and `shutdown_tick` shows only toasts (`nos_main.c:761`, `:771`, `:778`). The side-button cancel (`:2025`) is not announced anywhere. Impact: the user sees a blank screen with no hint that a press cancels the shutdown.
13. **[low] No SOS, emergency or medical-ID entry.** A search of `nos/`, `core/` and `firmware/main/` for SOS, medical and emergency finds only the diagnostics queue (`nos/diag_log.c:81`). Impact: a common power-menu item is missing.
14. **[low] The hero can step by 1-2 % with display state.** The load allowance is +25 mV when awake and +10 mV asleep (`hal_esp32.c:361`), and `go_state` updates it (`nos_main.c:701`). The value is integer and is not persisted. Impact: the number can change when the panel changes state.
15. **[low] Simulator parity.** `sim/nos_sim.c:95-102` only toasts. SLEEP, RESTART and OFF change no state in the simulator. Impact: the sheet's paths can be checked only on device.
16. **[low] Doc comment and code disagree on zero cells.** The field comment at `sys_power.c:84` says "at least 1". At 0 % the code lights none (`:171`).
