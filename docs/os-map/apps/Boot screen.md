---
title: "Boot screen"
id: sys-boot
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Boot screen

Source: `nos/apps/sys_boot.c` (210 lines; screen record `scr_boot` at `nos/apps/sys_boot.c:209`). Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status is from source reading only.

## Summary

The boot screen is the one-shot start-up animation. It draws a 5x7 dot-matrix "NOS" from 49 white dots and one red period, then cuts to home, where the face's own build-in is the reveal. It owns no data. It reads no service getter, prefs key or sensor. Its only direct HAL calls are a PSRAM allocation (`hal_big_alloc`) and a sound request (`hal_sfx`). Its haptic tick goes through `nos_feedback`.

It is pushed once at start-up by `nos_apps_boot()` (`nos/apps/registry.c:67-75`). It pops itself after about 1.08 s, or earlier on any touch-down or side-button press. It persists nothing.

Two things in the code differ from the file's own comments. The duration is about 1.08 s, not "~1.4 s", because `NOS_FPS` is 40, not 30 (`nos/render_config.h:8`). The chime ignores the UI sound toggle (`nos/apps/sys_boot.c:101` versus `nos/nos.c:572`).

## Features

- Black start, then the wordmark dots burst out of the centre and scatter onto a loose ring (`EASE_STEADY`, 8 frames, `nos/apps/sys_boot.c:43`, `:119-122`). Each dot starts on frame 3 to 6 (`:42`, `:87`).
- Dots gather into their 5x7 cells over 12 frames (`:44`, `:88`, `:127-134`). Glyph cells: N 18 dots, O 16, S 15, so 49 particles (`:33-35`). The cap is `MAXDOT` 64 (`:40`).
- The red period clicks in at frame 29 on a 3-frame snap table (`:45`, `:197-199`; `MF_SNAP3` at `nos/motion.c:49`).
- A dot that moves more than 16 px in one frame leaves a ghost (`:48`, `:194`). The comment at `:12-14` says the ghost is 45 % ink.
- Exit: every dot shrinks on `EASE_ACCEL` from frame 37 (`:46`, `:187`), quantised to 1/8 steps (`:188`). The screen cuts to home at frame 43 (`:47`, `:178`).
- Sound: `hal_sfx(SFX_BOOT)` on enter (`:101`). The chime is a ~0.5 s recipe: noise swell, three square stairs C5 G5 C6, then a detuned G6 triangle pair (`firmware/main/ext/ext_sfx_synth.h:130-139`).
- Haptic: one `HAPTIC_TICK` at the period click, only when the haptics pref is on (`:177`; `nos/nos.c:560`, `:570`). The tick makes no sound (`nos/nos.c:572`).
- Skip: any touch-down (`IN_DOWN`), side-button press (`IN_BTN`) or long press (`IN_BTN_LONG`) finishes at once (`:203-205`).
- Reveal: `sys_home_build_in()` restarts the current face's build-in just before the cut (`:108`; `nos/apps/sys_home.c:278-283`).

Timing: the frame index is `floor(s_t * M_FPS + 0.5)` with `M_FPS` = `NOS_FPS` = 40 (`:63`; `nos/motion.h:25`; `nos/render_config.h:8`). Frame 43 is 1.075 s nominal. Frames 29 and 37 are 0.725 s and 0.925 s.

## Reads

Services (`svc_*`): none. `svc.h` is included (`nos/apps/sys_boot.c:23`) but no `svc_*` function is called.

Prefs (`prefs_*`): no direct call. `prefs.h` is included (`:25`). Indirect reads of `nos_prefs`:
- `nos_prefs.haptics` and `nos_prefs.sound`, inside `nos_feedback()` at the period click (`nos/nos.c:570`, `:572`).
- `nos_prefs.face`, inside `cur_face()` (`nos/apps/sys_home.c:271-276`), called from `sys_home_build_in()` at `:282` when the boot finishes.

HAL (`hal_*`):
- `hal_big_alloc(sizeof(particle) * MAXDOT)` once, in `layout()` (`:67`). Implementation: `heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM)` (`firmware/main/hal_esp32.c:1595-1598`).
- `hal_sfx(SFX_BOOT)` (`:101`). Implementation reads `A`, `s_task`, `A->volume` and `ext_shutdown_requested()` (`firmware/main/ext/ext_audio.c:358-364`).
- `hal_haptic` indirectly through `nos_feedback` (`nos/nos.c:570`; `firmware/main/ext/ext_haptic.c:114`).
- Not called: `hal_millis`, `hal_store_*`, IMU, RTC, battery, SD, radio or touch-driver functions.

Engine and shared state:
- `dt` from the frame loop (`:173-175`; `tick_screens(nos_dt)` at `nos/nos.c:2347`). It is clamped per frame (`nos/nos.c:2307`).
- Motion: `EASE_STEADY` and `EASE_ACCEL` (`nos/motion.c:270`, `:274`), `MF_SNAP3` (`:49`), `bezier_ease`, `mf_at`, `m_hash01`, `m_lerp`, `m_clamp`.
- Geometry: `UI_CX` and `UI_CY` = 233 (`nos/ui.h:46-47`). Drawing: `dot_disc` (`nos/dot.c:673`), `ui_smear_disc`, `ui_trail_reset` and `ui_trail_push` (trail struct at `nos/ui.h:291`).
- Input: the `nos_input.type` enum (`nos/nos.h:40-42`).

## Writes

Own static state only (`nos/apps/sys_boot.c:50-61`):
- `s_p`: PSRAM array of `particle` (`:57`, `:67`). It is allocated once and never freed. Each particle is about 64 B (estimated from the struct, not measured), so about 4 KB.
- `s_n` (49 particles), `s_t`, `s_popped`, `s_red_x` and `s_red_y`.
- Per-frame position and trail for each particle (`:114-142`; `ui_trail_reset` and `ui_trail_push` at `:139-140`).

Outbound effects:
- `hal_sfx`: queues `SFX_BOOT` in `A->req`, up to 4 entries (`firmware/main/ext/ext_audio.c:358-364`).
- One haptic pulse at the period click, if haptics are on (`nos/nos.c:570`).
- Home: `sys_home_build_in()` sets `H.enter_t0` and `H.c.enter = 0`, then calls the current face's `enter` (`nos/apps/sys_home.c:278-283`).
- Navigation stack: `nos_pop_with(TX_NONE)` pops the boot entry, runs `call_reveal(home)` and sets `s_force_full` (`nos/nos.c:397-401`, `:382-395`).

No write to prefs, SD or NVS.

## Background behaviour

- The boot screen starts no task and no timer. Its `tick` runs inside the UI frame: `nos_frame_run` calls `frame_run` (`nos/nos.c:2299`), which calls `tick_screens` (`nos/nos.c:2101-2109`, `:2347`). The nominal cadence is 40 Hz (`nos/render_config.h:8`). The presentation cap is the `fps_cap` pref (`nos/prefs.h:43`) or `NOS_PRESENT_FPS` 60 (`nos/render_config.h:12-14`).
- The frame clamp is 4 periods, 100 ms (`nos/nos.c:2307`). A slow frame stretches the animation; it does not skip it.
- Screen off: the PWR_OFF branch of the main loop does not call `frame_run` (`firmware/main/nos_main.c:2091-2103`, `continue` at `:2103`). `boot_tick` therefore does not run and the boot clock `s_t` stops. Services run instead, once per second with `dt` = 1.0 (`nos_main.c:2094`), through `services_tick` (`nos/apps/registry.c:62-66`, `:71`). This is read from the code and was not tested on a board. In practice the boot is about 1 s long.
- Other work during the animation:
  - `dotbake` task, pinned to core 1, priority 1, 3072 B stack, started at `firmware/main/nos_main.c:1950`. Its comment says it bakes dot masks "while the boot animation plays" (`:1948-1949`).
  - `exta` audio task, priority 3, 4096 B stack in PSRAM, not pinned (`firmware/main/ext/ext_audio.c:344-346`). It plays the chime. The `extf` file task is created at `:350`.
  - Haptic motor driven by LEDC on GPIO16 (`firmware/main/ext/ext_priv.h:33`; `firmware/main/ext/ext_haptic.c:98-110`).
- OTA validation: the same frame code counts good frames and calls `ota_mark_valid()` when the count reaches 90 (`firmware/main/nos_main.c:2227`). Boot frames count toward that total.

## Persistence

None. The boot screen reads and writes no file, NVS key or SD path. The prefs it indirectly reads are loaded earlier, in `firmware/main/nos_main.c`:
- `settings_load()` at `:1921` reads the legacy NVS blob `cfg` and sets the legacy backlight (`core/settings.c:36-66`). Its default brightness is 200 (`core/settings.c:16`).
- `prefs_load()` at `:1923` loads the current prefs (`nos/prefs.c:240-258`). It reads NVS key `nosprefs` for migration (`nos/prefs.c:196`) and SD `settings.<slot>.bin` (`nos/prefs_disk.c:32`). It runs before `nos_apps_boot()` at `:1947`.

The boot does not check `prefs_loading()` (`nos/prefs.h:67`; `nos/prefs.c:283-285`). It runs and finishes whatever the load state.

## Navigation

Opened from: only `nos_apps_boot()` (`nos/apps/registry.c:67-75`), called at start-up from `firmware/main/nos_main.c:1947`. `scr_boot` is declared once (`nos/apps/nos_apps.h:8`). No other screen opens it. The simulator skips it unless `--boot` is given (`docs/NOS_SIM.md:100`).

Stack: `[scr_home (root), scr_boot]`. The root is set at `nos/apps/registry.c:73`. The boot is pushed with `TX_NONE` (no transition) at `:74`.

Flags: `SCR_NOBACK` (`nos/apps/sys_boot.c:209`), so there is no swipe-back.

Input: `boot_input` consumes every event (`:203-207`). The engine's global side-button handler (`nos/nos.c:951-955`) runs only when the screen returns 0. During the boot a side-button press is therefore handled by the boot and never reaches `nos_home()` or the power sheet. `IN_DOWN`, `IN_BTN` and `IN_BTN_LONG` finish the boot. `IN_TAP`, `IN_LONG`, `IN_BTN_DOUBLE` and swipes are swallowed without skipping.

Exit: `finish()` (`:104-110`) runs `sys_home_build_in()` and then `nos_pop_with(TX_NONE)`. `nos_pop` takes its `TX_NONE` branch (`nos/nos.c:391-393`): it calls leave on the boot (none is defined), pops, calls `call_reveal(home)` and does an instant cut with no transition.

Opens from the boot: none.

## Gaps

1. **Boot chime ignores the UI sound toggle (medium).** `nos/apps/sys_boot.c:101` calls `hal_sfx(SFX_BOOT)` unconditionally. Every other UI sound is gated on `nos_prefs.sound` (`nos/nos.c:572`). A user with UI sounds off still hears the chime at each power-on.
2. **Saved speaker volume and haptic strength are not applied at boot (medium).** `firmware/main/ext/ext_audio.c:337` sets `A->volume = 70`, and `firmware/main/ext/ext_haptic.c:43` sets `s_strength = 80`. The only code that pushes `nos_prefs.volume` or `haptic_strength` to the HAL is the Settings reset (`nos/apps/set_settings.c:1767-1769`), the Settings slider (`:2093-2096`) and the Developer card (`nos/apps/set_diag.c:634-643`). The start-up path (`firmware/main/nos_main.c:1921-1947`) does not. After each power-on, the chime plays at 70 % and the tick at strength 80 until the user changes those settings. This also affects every other `hal_sfx` and `hal_haptic` call.
3. **Sound and haptic assume an unverified external strap (low).** `firmware/main/ext/ext_priv.h:29` sets `EXT_BOARD_OK` to 1 on B. Its pin map (`:33-44`) is noted as copied from `docs/strap_layout.html` (`:32`). `firmware/main/pins.h:64` sets `BOARD_HAS_AUDIO` to 0 on B. `firmware/main/ext/ext_audio.c:343` marks the speaker present with "cannot be probed; default on". CLAUDE.md says the B strap mapping still needs its own verification. On a bare B, the chime and tick are attempted with no audible or haptic result and no fault report.
4. **PSRAM allocation failure puts the red period in the top-left corner (low).** If `hal_big_alloc` returns NULL (`firmware/main/hal_esp32.c:1597`), `layout()` returns at `nos/apps/sys_boot.c:67-68` before it sets `s_red_x` and `s_red_y` (`:92-93`). `boot_draw` then draws the period at the static default (0, 0) (`:197-200`), with no check. The audit at `docs/audits/2026-10-21/report.txt:2701` says the failure path gives "an empty animation that still completes". That misses the period dot.
5. **Comments and docs give stale timing and counts (low).** `nos/apps/sys_boot.c:1` says "~1.4 s". `:41` says "30 fps". `:14` says "57 separate dots". The code uses `NOS_FPS` 40 (`nos/render_config.h:8`) and 49 glyph dots (`sys_boot.c:33-35`). The real length is frame 43 / 40 = 1.075 s, and the per-frame clamp is at `nos/nos.c:2307`. `docs/os-map/systems/Shell and faces.md:13` and `:22` repeat "~1.4 s" and "30 fps". Anyone retuning from the comments will mis-time the animation and its frame budget.
6. **The boot shows no real start-up state (low).** `nos/apps/sys_boot.c:173-181` finishes only on the timer or on a skip. `nos/apps/registry.c:67-75` takes no status input. `prefs_loading()` (`nos/prefs.h:67`) is not consulted. OTA validity is set after 90 counted frames with no visible sign (`firmware/main/nos_main.c:2227`). A missing SD card, prefs still loading, or a pending OTA check gives no sign during the ~1 s. There is no logo, progress, battery or charging state, or version identity. A modern watch boot usually shows at least one of these.
7. **The "~78 % during the animation" note is not supported past the first frame (low).** `firmware/main/nos_main.c:1921` runs `settings_load()`, which sets the legacy level 200 (`core/settings.c:16`). After every ON frame, `set_level(user_level())` runs (`firmware/main/nos_main.c:2225-2226`). `docs/os-map/systems/Settings and developer.md:345` says the animation can show at about 78 %. In this code only the window before the first UI frame is at the legacy level, and the animation runs at the user level. Not checked on hardware.
8. **The dot-mask bake depends on the boot's length, and a skip does not wait for it (low, inferred).** `firmware/main/nos_main.c:1948-1950` starts the `dotbake` task "while the boot animation plays". `nos/apps/sys_boot.c:203-207` ends the screen at once on any skip, at any frame. An early skip leaves the bake running on core 1 into home's first use, and nothing waits for it. Not measured.

## Related

[[Engine and scheduling]] (frame loop, `tick_screens`, nav stack), [[Shell and faces]] (home build-in and face reveal), [[Storage and persistence]] (prefs load before boot), [[Hardware and drivers]] (strap audio and haptic, PSRAM), [[Power and battery]] (PWR_OFF path, OTA frame counting), [[Services hub]] (`svc_init` and `background_init` run in `nos_apps_boot()` before the boot screen is pushed, `nos/apps/registry.c:69-70`).
