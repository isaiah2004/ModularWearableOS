---
title: "Incoming call screen"
id: sys-call
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Incoming call screen

Source: `nos/apps/con_call.c` (500 lines), registered as `scr_call` (`nos/apps/con_call.c:499-500`). The call data and the ringer live in the services hub, `nos/svc.c`. Read-only source review; nothing built, flashed or run on a board, and the serial port was not touched. Evidence is `repo-relative/path:line`. Where a comment and the code disagree, the code is cited.

## Summary

The incoming call screen is the only UI for a phone call. It is not a launcher app. The services push it when the Gadgetbridge link reports an incoming call (`nos/svc.c:659-681`, `:975-983`). It has three states: `CS_INCOMING`, `CS_ACTIVE` and `CS_ENDED` (`nos/apps/con_call.c:45`).

- **Incoming:** caller name, a DECLINE and an ACCEPT button, and a bezel ring with a comet.
- **Active:** a duration timer, the caption "ON PHONE", and one END button.
- **Ended:** "CALL ENDED", or "MISSED CALL" when the phone stopped ringing first. The screen then pops itself.

The watch has no call audio. ACCEPT, DECLINE and END are one-line JSON messages to the phone (`nos/svc.c:943-965`). The header says so (`nos/apps/con_call.c:3-4`), and the code matches it. The screen stores nothing: its state is the static struct `C` (`con_call.c:48-75`), and the call itself is the services struct `s_call` (`nos/svc.c:79`).

Hardware it needs: the 466 x 466 panel, the FT3168 touch at 0x38 (B branch of `firmware/main/pins.h`), the BLE radio for the phone link, and the side button. The ring's haptic motor and speaker come from the external strap, which is not verified on B (see Gaps). It uses no IMU, RTC, SD, Wi-Fi, GPS or battery reading.

Status is **partial**, from source reading only. The state machine, buttons, ring and phone messages are implemented. Several user-facing gaps are real (see Gaps).

Doc drift (the code wins):
- `docs/NOS.md:32` lists "Call" under Connect. The launcher has no Call entry (`nos/apps/registry.c:8-36`). The screen opens only from the ring path.
- `docs/os-map/systems/Services hub.md:98` says swiping the ringing screen away drops the call state. The swipe is blocked (`nos/apps/con_call.c:500`, `nos/nos.c:852`, `:861`). The drop really happens through `nos_home()`, for example from the power sheet's Sleep button (see Navigation).
- `nos/apps/con_call.c:37` says the comet lap is 1.6 s. At `NOS_FPS` 40 (`nos/render_config.h:8`), `LAP_F` 48 frames is 1.2 s.
- `docs/PHONE.md:56` lists `call incoming / start / end / reject`. The code also handles `outgoing`, `accept`, `missed` and `ignore` (`nos/svc.c:1058-1067`).

## Features

Incoming (`CS_INCOMING`):
- Title "INCOMING CALL" (`con_call.c:351`) and caption "ANSWERS ON PHONE" (`con_call.c:380`).
- Caller name in up to two title lines of 340 px (`con_call.c:31-33`, `:99-125`, `:341-346`). Line 2 is clipped with "..." (`ui_text_fit`, `:346`). Line 1 is not clipped (see Gaps).
- The number appears on its own line only when the name and the number are both set and differ (`con_call.c:347-348`).
- Two 88 px round buttons at x = centre +/- 82 and y 326 (`con_call.c:28-30`, `:36`, `:381-382`). DECLINE is a dark disc with the screen's one red X (`:324-325`, `:330-331`). ACCEPT is a white disc with the phone glyph. Labels sit at y 396 (`:383-384`).
- A 60-dot ring at radius 222 px builds in on entry (`con_call.c:26-27`, `:247-283`). A comet laps it at 2 dots per frame (`:212-218`, `:285-292`). This is decorative; the sim motion audit marks it so (`:177-178`).
- The side button silences the ringer while incoming (`con_call.c:487`, `nos/svc.c:687`).
- Tap ACCEPT: sends ACCEPT, plays FB_SUCCESS, moves to ACTIVE on the same screen (`con_call.c:472-475`).
- Tap DECLINE: sends REJECT, plays FB_TAP, pops the screen (`con_call.c:466-471`).
- Hit radius for buttons is 58 px (`con_call.c:438-442`).

Active (`CS_ACTIVE`):
- Title "CALL" (`con_call.c:351`). Duration is MM:SS, or H:MM:SS from one hour (`con_call.c:149-154`). It counts from `t0_ms` (`:220-232`), so it is correct after a redraw.
- Digits morph over 6 frames when the second changes (`con_call.c:226`, `:231`, `:391`).
- Caption "ON PHONE" (`con_call.c:393`).
- The bezel ring fills with the seconds of the current minute, with a white marker (`con_call.c:274-280`).
- One END button at y 346 with the label "END" at y 416 (`con_call.c:29`, `:405-406`). Tap sends END and goes to ENDED (`:476-479`).
- On the incoming-to-active change, DECLINE slides to centre and becomes End, and ACCEPT folds away (`con_call.c:387-405`).

Ended (`CS_ENDED`):
- The name stays on screen, the timer is frozen and greyed, and "ON PHONE" and END fold away (`con_call.c:423-431`). Then the caption "CALL ENDED" comes up (`:431`).
- MISSED CALL: when the call ended while still ringing, both buttons dissolve to dots over 6 frames (`con_call.c:409-421`, `OUT_F` at `:35`). The caption reads "MISSED CALL" (`:418`).
- The screen pops itself 1.4 s after entering ENDED (`con_call.c:40`, `:233-236`), once no transition is running.

Not implemented: call audio, speaker, mute, hold, call waiting, quick reply, reject with a message, contact photo or lookup, call history, and any outgoing-call screen. See Gaps.

## Reads

Services getters (`nos/svc.h:162-167`):
- `svc_call()` returns `const svc_call_t *` with `state` (0 none, 1 incoming, 2 active), `name[32]`, `number[24]`, `t0_ms` (`nos/svc.h:162`). Read in `go()` (`con_call.c:93`), `call_enter()` (`:129`) and every tick (`call_tick`, `:198`). So the screen follows the phone state on each tick.
- `svc_call_generation()` (`con_call.c:131`, `:199`). A new call while the screen is up changes the generation and re-runs `call_enter()` (`:199`).

HAL:
- `hal_millis()` (`con_call.c:94`, `:144`, `:222`) is the duration clock: `esp_timer_get_time()/1000` (`firmware/main/hal_esp32.c:111-114`). It is the only `hal_*` call in the file.

Preferences:
- None read in `con_call.c`. The file includes `prefs.h` (`:20`) and uses no `nos_prefs` or `prefs_*` symbol.
- Indirect: `nos_feedback()` reads `nos_prefs.haptics` and `nos_prefs.sound` for the button taps (`nos/nos.c:557-573`).
- Indirect: the ringer reads `nos_prefs.sound` for the call tone (`nos/svc.c:701`). It does not read `haptics` or `dnd` (see Gaps).

Engine (timing and navigation, not data): `nos_t`, `nos_enter_t()` (`con_call.c:133`), `nos_transitioning()` (`:233`), `nos_pop()`, `nos_keep_awake()` (`:197`).

Services internals the screen relies on (not called from the file): `ring_screen()` and `call_find_tick()` read `s_call.state` and the screen's pushed flag (`nos/svc.c:659-681`, `:975-983`). The ringer reads `s_call.state` and `s_find_watch`, `s_ring_alarm`, `s_timer` for kind selection (`nos/svc.c:688-691`).

## Writes

Phone link (through `txf`, `nos/svc.c:150-156`, which drops the line silently when the phone is not connected):
- `svc_call_accept()` (`nos/svc.c:943-949`) returns early unless state is 1. It sends `{"t":"call","n":"ACCEPT"}`, sets `s_call.state = 2` and sets `t0_ms = hal_millis()`.
- `svc_call_reject()` (`nos/svc.c:951-956`) sends `{"t":"call","n":"REJECT"}` when state is 1, else `"END"`. It sets state 0.
- `svc_call_hangup()` (`nos/svc.c:958-964`) sends `{"t":"call","n":"END"}` and sets state 0.
- None of the three reports failure to the screen.

Services RAM:
- GB `call` lines write `s_call` and `s_call_generation` (`nos/svc.c:1044-1071`). See Persistence.
- Phone disconnect clears `s_call.state` (`nos/svc.c:1225`).
- `svc_ring_silence()` sets `s_ring_silenced = 1` and stops haptic and sound (`nos/svc.c:687`). The flag clears when the ring kind changes (`:695`).
- `ring_screen()` pushes the screen with TX_RISE (`nos/svc.c:665`) and pops it (`:676`).
- `call_find_tick()` sets `s_call.state = 0` when the screen was dropped while state was 1, with no message to the phone (`nos/svc.c:977-980`).

Output (ringer, `nos/svc.c:688-705`): every 2000 ms while incoming, `hal_haptic(HAPTIC_ALARM)` (`:699`) and `hal_sfx(SFX_NOTIFY)` (`:701`). The sound is skipped when `nos_prefs.sound` is off (`:701`).

Output (buttons): `nos_feedback(FB_TAP)` on DECLINE, END and silence; `FB_SUCCESS` on ACCEPT (`con_call.c:468`, `:474`, `:478`, `:487`).

Engine: `nos_pop()` (`con_call.c:235`, `:471`, `:482`), `nos_keep_awake()` (`:197`).

Diagnostics (services, not the screen): `action_trace` records DIAG_DOMAIN_PHONE START on accept (`nos/svc.c:946`), REJECT (`:954`), STOP on hangup (`:961`) and RECEIVE on each state change (`:1069-1070`). The screen writes no diagnostic record itself. `nos/nos.c:950` records every button event generically.

Nothing in this screen writes preferences, NVS, SD or `hal_store`. `C` is cleared on every entry (`con_call.c:130`).

## Background behaviour

What runs when the screen is closed or the panel is off, and who runs it:

- **Call state and ringer (services).** `svc_tick()` runs `phone_tick` (line parsing), then `call_find_tick` and `ringer_tick` (`nos/svc.c:1303-1328`). It is called from `services_tick()` (`nos/apps/registry.c:62-66`) and from the frame run, through `nos_bg_tick` (`nos/nos.c:2341`). It runs in the UI frame loop.
- **Frame rate.** With the screen awake, this runs at `NOS_FPS` 40 (`nos/render_config.h:8`). Up to 8 Gadgetbridge lines are parsed per tick (`nos/svc.c:49`, `:1236-1240`).
- **Panel off (PWR_OFF).** The loop runs `nos_bg_tick(1.0f)` once a second and skips frames (`firmware/main/nos_main.c:2091-2097`). So line parsing, the ringer and `call_find_tick` all run at 1 Hz, and the ring cadence and call latency are quantised to about 1 s.
- **Waking for a call.** In PWR_OFF, after the services tick, the loop checks `nos_top()`. If the top screen has SCR_KEEPAWAKE, it switches to PWR_ON (`nos_main.c:2094-2096`). The call screen has that flag (`con_call.c:500`), so an incoming call wakes the panel within about 1 s.
- **AOD and sensor modes.** The same services run at 1 Hz through `nos_frame_run`, and the same keep-awake check wakes the panel (`firmware/main/nos_main.c:2106-2121`).
- **Ring output.** The ringer only kicks the haptic and audio drivers. Haptic patterns play as an `esp_timer` one-shot chain (`firmware/main/ext/ext_haptic.c`, `PATS` at `:34-37`; `P_ALARM` loops at `:36`). `hal_sfx` queues a request to the `exta` audio task, which runs at priority 3 with a PSRAM stack, and the I2S channels are removed 1.5 s after the last need (`firmware/main/ext/ext_audio.c:1-20`, `:330-343`, `:358-364`).
- **Phone link.** The NimBLE host task receives Gadgetbridge bytes whenever `nos_prefs.ble` is on (default 1, `nos/prefs.c:151`). The radio enable is applied every main-loop pass (`firmware/main/nos_main.c:953-960`, `:1987`), not by screen state. So the link stays up with the panel off.
- **Keep-awake.** While the screen is on top with SCR_KEEPAWAKE, `power_timeout_due()` never fires (`nos/power_policy.h:18-19`, `firmware/main/nos_main.c:2080-2085`). The ringer also calls `nos_keep_awake()` each tick (`nos/svc.c:704`), and the screen calls it on every tick (`con_call.c:197`).
- **No ring timeout.** `RING_MAX_MS` (60 s) stops alarm, timer and find-watch rings only (`nos/svc.c:47`, `:848`, `:905`, `:981`). An incoming call that the phone never ends rings and keeps the display on until the user declines.
- **POWER OFF.** Deep sleep (`firmware/main/nos_main.c:811`). No call can arrive.

## Persistence

- Nothing from a call is saved. `C` is static RAM (`con_call.c:48-75`), cleared on each entry (`:130`). `s_call` and `s_call_generation` are RAM (`nos/svc.c:79`, `:85`). Reboot, restart and POWER OFF clear them. The services hub says the same (`docs/os-map/systems/Services hub.md:207`).
- There is no call log, missed-call list or call history. `gb_call()` posts no notification (`nos/svc.c:1044-1071`).
- Diagnostics: call actions are logged through `diag_log_event()`, which writes the RTC_NOINIT retained ring (`nos/diag_log.c:73-74`, `:212`, `:218`) and the diagnostics queue. The SD directory on the ESP build is `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`). The drain path that writes that directory was not traced.
- Settings that shape the call output are saved by the prefs store: key `nosprefs` through `hal_store` with NVS fallback (`nos/prefs.c:196`), and SD slots `watch/settings.N.bin` (`nos/prefs_disk.c:32`). Fields: `haptics` (`nos/prefs.h:9`), `haptic_strength` (`:10`), `sound` (`:11`), `volume` (`:12`), `dnd` (`:19`), `ble` (`:24`), `knock_lock` (`:31`). Defaults: haptics 1, strength 70, sound 1, volume 60, ble 1, knock_lock 1 (`nos/prefs.c:138-158`).
- Saved output levels are not re-applied at boot. `hal_audio_volume()` and `hal_haptic_set_strength()` are called only from Settings (`nos/apps/set_settings.c:1768-1769`, `:2093-2096`) and the diagnostics screen (`nos/apps/set_diag.c:634-643`). The drivers start at their own defaults, volume 70 (`firmware/main/ext/ext_audio.c:337`) and strength 80 (`firmware/main/ext/ext_haptic.c:43`). So after a reboot the call tone and buzz use 70 and 80, not the saved 60 and 70, until the user moves a slider.

See [[Storage and persistence]] for the prefs and diagnostics stores.

## Navigation

Opens from:
- The services only. `call_find_tick()` calls `ring_screen(&R_CALL, ...)` (`nos/svc.c:975-983`). `R_CALL` is `{ &scr_call, ... }` (`nos/svc.c:649`), and the push is `nos_push(r->s, TX_RISE)` (`:665`).
- The trigger is GB `call` with `cmd` `incoming` (`nos/svc.c:1051-1057`). While the screen is already pushed, a `start` or `accept` also keeps it up (`:977`).
- Not reachable from the launcher. `nos_app_list` has no Call entry (`nos/apps/registry.c:8-36`). A double press reopens only launcher apps (`nos_open()` at `registry.c:45-50`), so it cannot reach this screen.
- Not in the Control Centre, the pager or any face. No other production file references `scr_call` except `nos/svc.c:649-652`, the extern at `nos/svc.h:188`, and the simulator (`sim/nos_sim.c:84`). A unit test at `tests/svc_identity_weather_test.c:14` stubs it.
- It pushes no other screen.

Exits:
- DECLINE tap: `svc_call_reject()`, then `nos_pop()` (`con_call.c:466-471`).
- ACCEPT tap: state ACTIVE on the same screen (`con_call.c:472-475`).
- END tap: `svc_call_hangup()`, then ENDED, then pop after 1.4 s (`con_call.c:476-479`, `:233-236`).
- Phone-side `end`, `reject`, `missed` or `ignore` while ringing: ENDED with MISSED CALL, then pop (`nos/svc.c:1066-1067`, `con_call.c:207-209`, `:91`).
- Side button while incoming: silences the ringer and stays on the screen (`con_call.c:487`).
- Side button while active or ended: `call_input()` returns 0 for IN_BTN (`con_call.c:488`), so `dispatch_btn()` calls `nos_home()` (`nos/nos.c:952`). The screen leaves the stack. `ring_screen()` then drops it without a state change (`nos/svc.c:667-669`), and `call_find_tick()` clears state only for state 1 (`:977-979`). The call keeps running with no screen, no timer and no End button. Nothing else reads `svc_call()` (grep of `nos/`).
- Power sheet: Sleep, Restart and Off call `nos_home()` (`nos/apps/sys_power.c:352-358`, and the auto action at `:185`). That collapses the stack under the power sheet. For a ringing call, `ring_screen()` drops the screen and `call_find_tick()` sets state 0 without a REJECT (`nos/svc.c:977-979`). The phone keeps ringing. This is the real route behind the Services hub's "swipe away" note.
- Swipe back: blocked by SCR_NOBACK (`con_call.c:500`; `nos/nos.c:852` and `:861`). The rising-sheet close path is also gated by SCR_NOBACK at `nos/nos.c:861`.
- Long press: the power sheet covers this screen (`nos/nos.c:953-956`) and the call screen stays underneath.
- Another ring screen (alarm, timer, find-watch) covers it. `is_ring()` keeps it pushed (`nos/svc.c:650-652`, `:667`).
- Cover and reveal: `call_cover()` calls the empty `call_leave()` (`nos/nos.c:207`, `nos/apps/con_call.c:147`). On reveal, `call_enter()` clears `C` (`con_call.c:127-130`). So a call that ended under a cover reveals as "CALL ENDED", not "MISSED CALL".

Related: [[Find phone]], [[Alarms]], [[Timer]] (the other ring screens and their services path).

## Gaps

1. **High. Do Not Disturb does not silence calls.** Evidence: `nos/svc.c:688-705` (`ringer_tick`) never reads `nos_prefs.dnd`. DND gates only banners (`nos/svc.c:230`) and the notification buzz (`nos/nos.c:519`); the field is `nos/prefs.h:19`. User impact: with DND on, the wrist still buzzes every 2 s and plays the tone while a call rings.

2. **High. A ringing call never times out and keeps the display on.** Evidence: `RING_MAX_MS` applies to alarm, timer and find only (`nos/svc.c:47`, `:848`, `:905`, `:981`). `ringer_tick` and `call_find_tick` have no limit (`nos/svc.c:688-705`, `:975-983`). The ringer and the screen both keep the device awake (`nos/svc.c:704`; `nos/apps/con_call.c:500`; `nos/power_policy.h:18-19`). User impact: a call whose end message never arrives (while the link stays up) rings and keeps the screen on until the user taps DECLINE. That costs battery.

3. **Medium. The Haptics switch does not silence the ring.** Evidence: `nos/svc.c:699` calls `hal_haptic(HAPTIC_ALARM)` with no prefs check. The only haptic on/off check is in `nos_feedback()` (`nos/nos.c:570`). Haptic strength does apply to the ring (`firmware/main/ext/ext_haptic.c:126`). User impact: a watch with haptics off still buzzes for calls.

4. **Medium. The saved volume and haptic strength are not applied at boot.** Evidence: the only setters are in Settings and the diagnostics screen (`nos/apps/set_settings.c:1768-1769`, `:2093-2096`; `nos/apps/set_diag.c:634-643`). The drivers default to volume 70 and strength 80 (`firmware/main/ext/ext_audio.c:337`, `firmware/main/ext/ext_haptic.c:43`), while the prefs default to 60 and 70 (`nos/prefs.c:139-141`). User impact: after every reboot the call tone and buzz use the driver defaults until the user moves a slider.

5. **Medium. On a bare B the ring may be neither felt nor heard, and nothing says so.** Evidence: the strap drivers are compiled for B (`firmware/main/ext/ext_priv.h:24-29`). The motor is on GPIO16 (`:33`) and the speaker on I2S and amp pins 1, 2, 3, 5, 6 (`:34-38`). Both are marked present without a probe (`firmware/main/ext/ext_haptic.c:107-109`, `firmware/main/ext/ext_audio.c:343`). B has no built-in audio (`firmware/main/pins.h:64`, `BOARD_HAS_AUDIO 0`). `CLAUDE.md` says the B strap needs its own verified mapping. User impact: without the strap the screen still shows INCOMING and the ringer runs, but nothing is felt or heard, and no indicator warns the wearer.

6. **Medium. Accept, Decline and End do nothing while the watch is locked.** Evidence: while `nos_locked` is set, a new press is ignored and only the hint toast shows (`nos/nos.c:666`). The lock is set and cleared only by the knock detector (`nos/nos.c:636-652`). Nothing clears it for a ringing call, and `con_call.c` has no lock check. `knock_lock` defaults to 1 (`nos/prefs.c:158`). User impact: with knock-to-lock on (the default), the buttons do nothing until the user knocks twice, and the only response is the "KNOCK TWICE TO UNLOCK" toast.

7. **Medium. An active call cannot be ended or seen once its screen is left.** Evidence: the side button in ACTIVE calls `nos_home()` (`nos/nos.c:952`; `con_call.c:487-488`). The screen is dropped (`nos/svc.c:667-669`), and state stays 2 because `call_find_tick` clears only state 1 (`:977-979`). `scr_call` has no other entry point (`nos/svc.c:649`; `sim/nos_sim.c:84` only). User impact: the call goes on with a timer nobody can see, no END, and no indicator on any other screen.

8. **Medium. Sleep, Restart or Off from the power sheet silently declines a ringing call.** Evidence: `nos/apps/sys_power.c:352-358` (Sleep calls `nos_home()`, then `nos_host_power(0)`) and `:185` (the auto action calls `nos_home()`). `nos/svc.c:977-979` clears state with no `txf`. User impact: the phone keeps ringing, the watch sends no REJECT, and the user is not told. The Services hub describes this as "swiping away", which is not the real path.

9. **Medium. Outgoing calls and phone-started calls get no screen.** Evidence: `ring_screen` wants the screen only for state 1, or for state 2 while already pushed (`nos/svc.c:977`). GB `outgoing`, `start` and `accept` only set state 2 (`nos/svc.c:1058-1065`). `docs/PHONE.md:56` says only incoming opens the screen. User impact: a call the user places on the phone has no timer, no caption and no End on the watch.

10. **Medium. Declines and phone-side ends read as MISSED CALL.** Evidence: GB `end`, `reject`, `missed` and `ignore` all set state 0 (`nos/svc.c:1066-1067`). ENDED sets `missed` whenever the previous state was INCOMING (`con_call.c:91`). The caption is drawn at `:418`. A link drop while ringing also clears state (`nos/svc.c:1225`), and the same logic marks it missed (`con_call.c:209`). User impact: a call declined on the phone reads as missed, and the watch cannot tell the two apart.

11. **Medium. Missed calls are not kept.** Evidence: `gb_call()` posts no notification and writes no record (`nos/svc.c:1044-1071`). ENDED pops after 1.4 s (`con_call.c:40`, `:233-236`). Mitigation: if Gadgetbridge forwards the dialer's own notification, it arrives as a generic card with the phone icon (`nos/svc.c:179`). User impact: a missed call is visible for about 1.4 s, then it is gone. There is no list.

12. **Medium (feature). No call audio on B, and the screen only tells the phone.** Evidence: the header states the design (`con_call.c:3-4`), and ACCEPT only sends a message (`nos/svc.c:943-950`). `CLAUDE.md` records that B has no built-in audio. User impact: accepting a call does not connect audio to the watch. Quick reply, mute and speaker are missing.

13. **Low. A second incoming call overwrites the active call.** Evidence: incoming sets state 1 and resets `t0_ms` with no check for an active call (`nos/svc.c:1051-1057`). `call_tick` moves the screen to INCOMING (`con_call.c:207`). User impact: the active timer is lost, and the first call disappears from the watch. There is no hold.

14. **Low. A caller identifier can go stale.** Evidence: `outgoing`, `start` and `accept` overwrite `name` and `number` only when at least one is non-empty (`nos/svc.c:1061-1064`). Otherwise the previous call's values remain in `s_call`. User impact: a call placed or answered with no identifiers shows the last caller's name.

15. **Low. A long single-word name can run past its width.** Evidence: `wrap_name()` keeps the whole line 1 when no space or hyphen is found (`con_call.c:107`, `:117`). Line 1 is drawn with `ui_text()`, not `ui_text_fit()` (`:345`). Line 2 is fitted (`:346`). User impact: a long name with no break can exceed the 340 px name width. Not verified on the glass.

16. **Low. Covering the screen during a call can change the caption.** Evidence: `call_enter()` clears `C` on reveal (`con_call.c:127-130`), and `missed` is set only in `go()` (`:91`). User impact: a call that ended while an alarm covered the screen reveals as "CALL ENDED", not "MISSED CALL".

17. **Low. Accept, Decline and End are fire-and-forget.** Evidence: `txf()` returns nothing (`nos/svc.c:150-156`). `svc_call_accept()` sets local state before any phone reply (`:948`). No inbound ACCEPT, REJECT or END acknowledgement is handled. Repo evidence does not show what Gadgetbridge does with these messages. User impact: the watch can show ON PHONE and run the timer even if the phone did not act.

18. **Low (feature). No contact lookup or photo.** The name is whatever the phone sends (`nos/svc.c:1051-1057`). User impact: callers appear as raw names or numbers.

19. **Low (doc). Comment drift.** The 1.6 s comet lap comment is wrong at 40 fps (`nos/apps/con_call.c:37`, `nos/render_config.h:8`). `docs/NOS.md:32` lists Call under Connect, which is not a launcher entry. Both are documentation only.

Related notes: [[Services hub]], [[Connectivity and voice]], [[Shell and faces]], [[Engine and scheduling]], [[Power and battery]], [[Storage and persistence]], [[Hardware and drivers]].
