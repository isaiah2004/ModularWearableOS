---
title: "Music"
id: app-music
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Music

Source: `nos/apps/con_music.c` (969 lines). Read-only source review; nothing built, flashed or run on a board, and the serial port was not touched. Evidence is `repo-relative/path:line`. Where a comment and the code disagree, the code is cited.

## Summary

Music is the NOS remote control for the phone's media player. It shows the track title, artist, elapsed and remaining time and a progress ring, offers previous, play/pause and next, and changes volume. The watch plays no audio itself. Track data arrives over the Gadgetbridge NUS link as `musicinfo` and `musicstate` lines (`nos/svc.c:1156-1173`) and is held in RAM by the service layer (`nos/svc.c:65-68`). Commands go back to the phone as `{"t":"music","n":...}` (`nos/svc.c:291-299`). The screen owns no sensor, no storage and no settings. Its own state is static RAM (`nos/apps/con_music.c:79-86`, `:241-288`, `:801-817`).

It is also Home widget slot 5 (`nos/apps/sys_home.c:5`, `:203`; `nos/apps/con_music.c:969`). A tap on the widget's centre disc toggles play/pause. A tap anywhere else opens the full app (`con_music.c:953-967`).

Status is **partial** from source reading. The Gadgetbridge path is complete end to end. Volume is open loop because the phone's level is never read (see Gaps). The Wi-Fi/companion media source is hidden by the link gate. Nothing was confirmed on hardware in this pass.

Related: [[Services hub]], [[Connectivity and voice]], [[Engine and scheduling]], [[Shell and faces]], [[Power and battery]], [[Hardware and drivers]], [[Storage and persistence]], [[Sports and workouts]].

## Features

- **Now-playing page.** Title in white, artist in secondary grey (`con_music.c:672-675`). Long titles scroll as a marquee at exactly 1 px per frame, with a 60-frame hold at rest and a 56 px gap. The clip window is 300 px wide (`:150-153`, `:489-495`, `:542-572`).
- **Times.** Elapsed on the left, remaining on the right, in `m:ss` or `h:mm:ss` (`:127-132`, `:302-307`). Remaining shows `--:--` when the duration is unknown (`:306`, `nos/ui.h:187`).
- **Progress ring.** 40 radial ticks at radius 219 px hug the bezel (`:135-138`, `:168-207`). Played ticks light up, and the leading tick steps its ink in 8 steps. There is no playhead and no scrubbing (`:168-173`).
- **Transport.** Play/pause in a white disc at the centre (`:685-692`). Previous and next sit 106 px either side (`:148`, `:680-681`). Hit testing is in `ctrl_at` (`:703-713`). Taps are dispatched only while `P.have` is true (`:744`, `:748`).
- **Volume.** A pill with a 10-segment bar replaces the times for 1.8 s after a change (`:154`, `:512-519`, `:601-615`). It is set by air up/down (`:782-783`) or by vertical drag at 26 px per 10 % (`:755-770`). Each 10-unit step sends one `volumeup` or `volumedown` (`:715-725`).
- **Skip by air.** Air left is previous and air right is next (`:784-785`). The glyph nudges 4 px for 4 frames (`:727-736`, `:524`).
- **Empty states.** "Nothing playing / Play music on your phone" and "Phone not connected / Pair in Gadgetbridge as Bangle.js NOS" (`:617-623`). The page swaps with a 2-frame out-fade and a build-in when the phone or player goes away or returns (`:35-69`, `:445-449`).
- **Fresh-link hold.** For the first second after a link the page holds the last state. A new title must stand 0.2 s before it is accepted, so the play state is in before the first frame (`:92-106`).
- **Optimistic play/pause.** The icon flips at once on tap. The phone's echo settles it, or after 2 s of silence it reverts to the phone's state (`:113-116`, `:119-125`).
- **Track-change animation.** The old words rise out and the new words rise in. The direction follows the last tap, so previous falls and next rises (`:453-486`).
- **Equaliser.** An 11 x 5 dot matrix at the top (`:139-142`). It is decorative: levels come from a hash plus a beat term, not from audio (`:369-382`). It lies flat when paused (`:376`).
- **Home widget.** A compact ring of 36 ticks at radius 88 px (`:798-799`), a play/pause disc, and the title and artist (`:889-945`). Tapping the disc toggles play/pause. Any other tap opens the app with an iris from the disc (`:953-967`).

## Reads

Service getters:
- `svc_music()` (`nos/svc.h:62`, `nos/svc.c:277`). Read in `player_poll()` (`con_music.c:88-117`) on every app tick (`:443`) and every Home widget tick (`:846`, called from `nos/apps/sys_home.c:635`). Fields used: `have`, `track`, `artist`, `playing`, `pos_ms`, `dur_ms`, `volume` (`con_music.c:98-115`, `:348-349`, `:510-511`). `album` is in the struct (`nos/svc.h:57`) but this screen never reads it.
- `svc_sys()->phone_connected` (`con_music.c:91`; `nos/svc.h:181`, written at `nos/svc.c:1220`).

HAL and driver calls:
- `hal_phone_connected()` (`con_music.c:91`). On the ESP32 build it is true only when BLE is enabled, the clock is ready, the NUS TX characteristic is subscribed, and a central is connected (`firmware/main/hal_esp32.c:1159-1162`).
- `hal_light_gesture_enable()` (`con_music.c:358`, `:428`, `:792`) and `hal_light_gesture()` (`:431`). These sit on the optional APDS-9960 driver (`firmware/main/ext/ext_light.c:250-266`).
- `hal_big_alloc()` (`con_music.c:897`). One PSRAM snapshot for the widget blur bake.

Engine and shell calls (timing and input, no data): `nos_t`, `nos_enter_t()`, `nos_top()`, `nos_transitioning()`, `nos_open()`, `nos_feedback()`, `nos_keep_awake()`, `nos_air()`, `nos_subview()` (`con_music.c:335`, `:427`, `:724`, `:525`, `:433`, `:567`, `:966`).

Prefs: none read directly. `nos/prefs.h` is included (`con_music.c:24`), but no `nos_prefs` or `prefs_*` symbol is used in this file. `nos_feedback()` does read `nos_prefs.haptics` and `nos_prefs.sound` (`nos/nos.c:557-570`).

Storage: none.

## Writes

Phone commands, all through `svc_music_cmd()` (`nos/svc.c:284-302`):
- Play/pause: `con_music.c:124`. The phone receives `{"t":"music","n":"play"}` or `"pause"` (`nos/svc.c:291-299`).
- Next/previous: `con_music.c:731`. The phone receives `"next"` or `"previous"`.
- Volume: `con_music.c:722`, one send per 10-unit step, `"volumeup"` or `"volumedown"`.
- Route: the GB text line is used when `hal_phone_connected()` and the source is not the companion (`nos/svc.c:290`). Otherwise `hal_media_cmd()` runs (`nos/svc.c:301`), which notifies MEDIA_CMD over BLE and sets `g_cmd_val` for Wi-Fi `GET /cmd` (`firmware/main/hal_esp32.c:1015-1023`, `:1013`, `:1767`).

Service-side optimistic state (`nos/svc.c:294-297`): `s_music.playing` flips and the position is reset to its current value (`music_set_pos`, `nos/svc.c:278-283`). The phone's `musicstate` settles it.

Diagnostics: `action_trace(DIAG_DOMAIN_MUSIC, DIAG_ACT_SEND, cmd, ..., DIAG_RESULT_PENDING, ...)` runs on every command (`nos/svc.c:289`, helper at `:102-106`). It writes an action record to the diagnostic log (`nos/diag_log.c`). The record is always PENDING, and nothing updates it. `DIAG_DOMAIN_MUSIC` has no other call site.

RAM only: `P` (`con_music.c:79-86`), `M` (`:241-288`), `G` (`:801-817`), and `M.vol` (`:349`, `:723`). The service copy is `s_music`, `s_music_src`, `s_music_pos0`, `s_music_t0` and `s_music_poll_ms` (`nos/svc.c:65-68`).

Haptic and sound: `nos_feedback()` on tap, open, toggle, limit and error (`con_music.c:724`, `:735`, `:750`, `:959`). It drives the haptic motor when `nos_prefs.haptics` is set and calls `hal_sfx` when `nos_prefs.sound` is set (`nos/nos.c:557-570`).

Sleep timer: `nos_keep_awake()` runs on every app tick while `P.playing` (`con_music.c:525`). It resets `s_last_input_ms` (`nos/nos.c:585`).

Gesture engine: enabled on entry and on each foreground tick while a title is present (`con_music.c:358`, `:428`). Disabled on leave (`:792`).

Sports side effect: every air gesture, from any screen, is also queued as an `SP_GESTURE` sports record (`nos/nos.c:2335`, `nos/sports.c:145-151`). Music does not cause this, but the same gesture event feeds it.

NVS, SD and prefs writes: none.

## Background behaviour

The app splits into two parts. The screen code runs only while the page is on glass. The service code runs all the time.

Screen code (`scr_music` tick, draw and input): `tick_screens()` calls it only for the top screen, or for the screens involved in a transition (`nos/nos.c:2101-2109`). Closed means not ticked. The nominal animation cadence is `NOS_FPS` 40 (`nos/render_config.h:8`). The device may present at up to 60 (`:11`).

Service code (`svc_tick()`, which runs `phone_tick()` and `music_tick()`): called from `nos_bg_tick`, which is `services_tick` (`nos/apps/registry.c:62-72`). The rate depends on the power state:
- display ON: once per frame of the render loop (`nos/nos.c:2341`).
- display OFF: once per second with `dt = 1.0` (`firmware/main/nos_main.c:2094`).
- AOD: once per second through `nos_frame_run` (`firmware/main/nos_main.c:2116`).

Per-tick music work in the service layer:
- Gadgetbridge lines are drained up to 8 per tick (`nos/svc.c:49`, `:1236`). They arrive in an 8 KiB NUS ring (`firmware/main/hal_esp32.c:1083`) filled on the NimBLE host task. That task is pinned to core 0 (`firmware/sdkconfig:709`) at priority `configMAX_PRIORITIES-4` (`firmware/main/hal_esp32.c:1531-1533`). With the display off this drain runs at 1 Hz, so at most 8 lines per second.
- Position interpolation for the GB source adds elapsed time to `pos0` while playing (`nos/svc.c:306-313`). The phone's `position` arrives in whole seconds (`nos/svc.c:1169`).
- Companion fallback: polls `hal_media_get()` and `hal_media_time()` at 250 ms (`nos/svc.c:315-332`) when no GB source is active.

Home widget: `wg_tick` runs on each Home frame for visible slots (`nos/apps/sys_home.c:635`) and calls `player_poll()` (`con_music.c:846`). It never calls `nos_keep_awake()`.

APDS-9960 gesture engine: the `ext` task (priority 0, no core pin, 4 KB stack, `firmware/main/ext/ext_sys.c:186-187`) services the sensor. The engine turns itself off after 8 s without `hal_light_gesture()` calls (`nos/hal_ext.h:35`, `firmware/main/ext/ext_priv.h:59`).

Screen off: the Music page does not run, so its draw and input do nothing. The service layer keeps the link, the track state and the interpolation running at 1 Hz. Music traffic never wakes the display. The wake predicate takes only touch, button, raise and knock (`nos/power_policy.h:16-17`, `firmware/main/nos_main.c:2003`).

Keeping the screen on: while Music is the top screen and `P.playing`, the sleep timer never expires. `nos_keep_awake()` is called every tick (`con_music.c:525`, `nos/nos.c:585`), and the sleep policy reads the same timer (`firmware/main/nos_main.c:2083-2084`). The app's `flags` are 0 (`con_music.c:793`), so the screen is kept on by a side effect, not by a declared keep-awake flag.

## Persistence

The app persists nothing. Track, artist, times, play state and volume live in RAM and are lost at reboot. After a reboot the phone must re-send `musicinfo` and `musicstate` on the next link (`nos/svc.c:1156-1173`).
- `M.vol` starts at 50 after boot (`con_music.c:348-349`). A volume the user sets is not saved.
- No NVS key, no SD path, no prefs key is written or read for music.
- Diagnostics: one PENDING record per command (`nos/svc.c:289`) goes into the diagnostic log (`nos/diag_log.c`). That log keeps retained copies in RTC memory (`nos/diag_log.c:73-74`). It writes to `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`, `:473-503`) when the SD card is available. This is the only persistence Music touches.
- The companion palette (`hal_media_colors`, `firmware/main/hal_esp32.c:944`) is not read by this screen.

## Navigation

Opens from:
- Launcher: "Music", the 13th entry (`nos/apps/registry.c:21`).
- Home page 5, the Music widget (`nos/apps/sys_home.c:5`, `:203`). Tap on the disc toggles play/pause. Any other tap calls `nos_open(&scr_music, UI_CX, WG_CY)` (`con_music.c:953-967`).
- Last-app double press. `nos_open()` records `s_last_app` (`nos/apps/registry.c:42-47`).

Opens: nothing. No control on the page pushes another screen (`con_music.c:738-790`).
Settings: no Music section. No `nos/apps/set_*.c` file mentions music.
Control Centre: no media tile. `nos/apps/sys_control.c` has no music reference.
Notifications: an app named "music" or "spotify" gets the music icon (`nos/svc.c:181`). Tapping the notification does not open this screen.

Back: the shell pops the screen (generic behaviour, not traced in this card).

## Gaps

Ordered by severity.

1. **Volume is open loop (high).** Every write path sets `volume = -1` (`nos/svc.c:325`, `:331`, `:1162`, `:1172`, `:1226`, `:1286`), and no path sets a real level. The app therefore shows its own guess, 50 after boot (`con_music.c:348-349`, `:510-511`). `set_volume()` sends `(v - M.vol)/10` steps, which assumes each phone step is 10 units and that the phone's level equals `M.vol` (`con_music.c:718-722`). Example: the phone is at 20 and the pill reads 50. One press of up shows 60, but the phone may go to 30 or to some other value. The repo does not show whether Gadgetbridge applies `volumeup`. The service comment says transport APIs send no delivery acknowledgment (`nos/svc.c:287-288`).

2. **Wi-Fi/companion media is hidden (medium, code reading only).** `player_poll()` shows the page only when `P.connected` (`con_music.c:91`, `:106`). That needs `hal_phone_connected()`, which requires NUS subscription and a BLE central (`firmware/main/hal_esp32.c:1159-1162`). A Wi-Fi `POST /media` (`firmware/main/hal_esp32.c:1759`, route `:1815`) fills `hal_media_get()` and the svc companion source (`nos/svc.c:319-332`). The app still shows "Phone not connected / Pair in Gadgetbridge as Bangle.js NOS" (`con_music.c:620`).

3. **No seek (medium).** The ring is display only, and the file says so: "no playhead" (`con_music.c:168-173`). `hal_media_seek()` exists (`firmware/main/hal_esp32.c:1025`; sim `sim/main_win32.c:188`, `sim/nos_hal_sim.c:375`), but no NOS code calls it, and `nos/svc.h:52-64` has no seek API.

4. **Album and artwork are never shown (medium).** `album` is parsed (`nos/svc.c:1159`) and stored (`nos/svc.h:57`), but nothing reads it. The companion palette (`hal_media_colors`, `firmware/main/hal_esp32.c:944`) is read only by the legacy `core/apps/app_music.c:503`, not by the NOS screen. There is no artwork path.

5. **No shuffle or repeat (low to medium).** The `musicstate` line carries `shuffle` and `repeat` (`sim/nos_phone_sim.c:87`, `tools/gb_test.c:80`). `gb_message()` ignores them (`nos/svc.c:1165-1173`, no match for `shuffle` or `repeat` in `nos/svc.c` or `nos/gb.c`). The app has no controls for them.

6. **The screen stays on while playing (medium).** `nos/apps/con_music.c:525` resets the awake timer on every tick while `P.playing`. The sleep check (`firmware/main/nos_main.c:2083-2084`) never fires until playback stops or the user leaves. The configured 5-60 s timeout (`nos/prefs.h:22`) is silently defeated. Power cost not measured.

7. **Air gestures need an optional module (medium, unverified on B).** Air input works only if an APDS-9960 answers at 0x39 when the ext subsystem probes at boot (`firmware/main/ext/ext_light.c:94-118`). When it is absent, `hal_light_gesture()` returns `GEST_NONE` (`ext_light.c:250-253`) and enable is a no-op (`:262-266`). Neither `firmware/main/pins.h` nor `docs/HARDWARE.md` lists this sensor for the 1.43-B. Air volume and air skip may therefore be dead on B.

8. **No standalone playback (medium, feature gap).** The watch cannot play music. The hardware table lists no built-in audio for the B (`CLAUDE.md`, "Built-in audio: Absent"), and there is no local library. The app is a remote only, which a user of a "Music" app may not expect.

9. **Widget has play/pause only (low).** `wg_tap()` toggles play/pause or opens the app (`con_music.c:953-967`). Next and previous are not on the Home widget.

10. **Equaliser is decorative and its cadence is misstated (low).** Levels come from `m_hash01()` plus a beat term (`con_music.c:369-382`), not from audio. The header says "dances while playing" (`:7-8`) and "10 Hz" (`:8`, `:364` comment). `eq_tick()` steps every 3 authored frames (`:364`), which is 13.3 Hz at `NOS_FPS` 40 (`nos/render_config.h:8`).

11. **Header copy is stale (low).** The header says `[ NOTHING PLAYING ]` and `[ NOT CONNECTED ]` (`con_music.c:14-15`). The code passes "Nothing playing" and "Phone not connected" (`:620`, `:622`), which `ui_absent()` draws unchanged (`nos/ui.c:576-584`).

12. **Play/pause and skip are optimistic with no delivery signal (low).** Play/pause shows at once and reverts after 2 s if the phone never echoes (`con_music.c:113-116`). Next and previous change nothing locally: the glyph nudges (`:729-736`), and the words change only when a `musicinfo` arrives. The service states that there is no delivery acknowledgment (`nos/svc.c:287-288`).

13. **Music diagnostics never resolve (low).** Each command is recorded as PENDING and the result is never updated (`nos/svc.c:289`). No other `DIAG_DOMAIN_MUSIC` call exists.

14. **One-second position resolution on the GB path (low).** `musicstate` `position` is whole seconds (`nos/svc.c:1169`). Elapsed time steps and then interpolates between updates (`nos/svc.c:306-313`).

15. **Display-off latency (low).** With the display off, GB lines drain at 1 Hz, 8 lines per call (`nos/svc.c:49`, `:1236`; `firmware/main/nos_main.c:2094`). A burst of other BLE traffic can delay `musicinfo` and `musicstate` by several seconds.

16. **No music settings (low).** There is no volume step size, marquee, equaliser or screen-on option. `nos/apps/set_settings.c` has no Music section.

## Related

- [[Services hub]]: `svc_music`, `svc_music_cmd`, GB and companion sources.
- [[Connectivity and voice]]: Gadgetbridge NUS lines, `musicinfo` and `musicstate`.
- [[Engine and scheduling]]: `nos_bg_tick`, screen lifecycle, frame timing.
- [[Shell and faces]]: launcher order, Home pager, widget slots.
- [[Hardware and drivers]]: APDS-9960 optional module, haptics.
- [[Power and battery]]: sleep timer and screen-on while playing.
- [[Storage and persistence]]: diagnostic log destinations.
- [[Sports and workouts]]: `SP_GESTURE` records fed by air gestures.
