---
title: "About"
id: sys-about
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# About

## Summary

About is the NOS information page for the 1.43-B build. It is one `nos_screen`, `scr_about` (name "ABOUT", title "ABOUT", `nos/apps/set_about.c:345`), that shows a dot-matrix logo, the version with a source build ID, the board name, a display line, free PSRAM and internal RAM, uptime, the sensed battery rail voltage, and the font credits.

- It has no inputs beyond scrolling. It opens nothing, has no child screens, and owns no task, timer or service registration.
- It reads no service getter and no prefs key. `set_about.c` contains no `svc_*`, `prefs_*` or `nos_prefs` call, although it includes `prefs.h` (`set_about.c:19`).
- It writes nothing to NVS, SD or prefs. All of its state is RAM (`static A`, `set_about.c:60-69`).
- Only uptime and the battery rail are live. They refresh about once per second of on-screen time (`set_about.c:121`, `:141`). The board, display and type rows are literals. The version row uses a build constant and the build ID.
- The battery rail is read without checking `sample_valid`, so a stale voltage can show with no marker (see Gaps).

Status is **partial**. The page renders and its two live rows update. The frame-rate line is a literal that disagrees with the default cap, and the board name is hard-coded.

Related: [[Services hub]], [[Hardware and drivers]], [[Power and battery]], [[Engine and scheduling]], [[Shell and faces]], [[Settings and developer]], [[Storage and persistence]].

## Features

Rows in page order. The whole list is a `ui_list` in 26 px units (`set_about.c:54`, `:112`).

- **Logo block** (`R_LOGO`, `set_about.c:53`). A 17 x 7 dot logo from the `k_logo` bitmap (`set_about.c:35-43`), drawn with `dot_disc` (`:185-196`). Each dot grows over four frames from a hashed start frame 0-8 (`logo_p`, `:168-173`; `MF_STEADY4`, `:48`). The tagline "NOTHING-INSPIRED WATCH OS" arrives with the first info row (`:236-238`).
- **Glint.** A diagonal band of dots swells 15 % as it crosses the logo, 1 s after entry and then every 5 s, for 1.2 s each time (`draw_logo`, `:179-184`; the same phase is repeated in the cost probe at `:153-156`).
- **VERSION** (`R_VERSION`, `:244`). `NOS_VERSION` plus the first 8 characters of `NOS_SOURCE_BUILD_ID`, for example `0.5.0-alpha  (xxxxxxxx)` (`:119`). `NOS_VERSION` is a hand-set macro (`nos/nos.h:27`). The build ID is the 16-character prefix of the source-tree SHA-256 written by `tools/build_identity.py:106-109` during `prepare`. On the simulator the ID is the word "simulator" (`set_about.c:31`).
- **BOARD** (`R_BOARD`, `:245-247`). Two literal lines, "ESP32-S3-Touch-" and "AMOLED-1.43-B". Not read from `WATCH_BOARD_NAME`.
- **DISPLAY** (`R_DISPLAY`, `:251`). Literal "466 x 466 AMOLED, 30 fps".
- **PSRAM FREE** (`R_PSRAM`, `:249`) and **INTERNAL RAM FREE** (`R_SRAM`, `:250`). Current free bytes from `hal_mem_info`, formatted as "x.x MB" or "NNN KB" (`fmt_mem`, `:80-84`).
- **UPTIME** (`R_UPTIME`, `:207`, `:93-97`). `H:MM:SS` under 24 h, then `nD HH MM`. Seconds are dropped after 24 h. Counted from boot, reset by any reboot.
- **BATTERY RAIL** (`R_RAIL`, `:208`, `:98-103`). Two decimals with a caps "V" label beside the digits (`:262-267`). Shows "--" (`UI_NONE`, `nos/ui.h:187`) when there is no value.
- **TYPE** (`R_FONTS`, `:252-255`). Literal "Space Grotesk, Space Mono" and "SIL OPEN FONT LICENSE 1.1". These are the families baked into `nos/fonts/` (`nos/fonts/nos_fonts.h:3-4`), whose licence texts are in `third_party/fonts/spacegrotesk/OFL.txt` and `third_party/fonts/spacemono/OFL.txt`.
- **Scrolling and fling.** Vertical drag scrolls the page with a scrollbar (`about_draw`, `:322-334`). Fling blur uses 90 px bands (`BAND_H`, `:276`; `band_tick`, `:297-307`). Rows rise 14 px on entry, one a frame from frame 7 (`row_bk`, `:214-223`).
- **Tap** does nothing except stop a fling (`about_input`, `:336-343`).

## Reads

Nothing from services or prefs. The reads are:

- **HAL**
  - `hal_power_get()`: `firmware/main/hal_esp32.c:378-388`. Declared `core/hal.h:37-41`. The About code reads only `rail_mv` (`set_about.c:98-100`). It ignores `sample_valid`, `battery_valid`, `external_power` and `age_ms`.
  - `hal_battery_mv()`: `hal_esp32.c:389`. Used only when `rail_mv` is 0 (`set_about.c:100`). Declared `core/hal.h:33`.
  - `hal_mem_info()`: `firmware/main/ext/ext_sys.c:207-212`, declared `nos/hal_ext.h:229`. Calls `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` and `MALLOC_CAP_INTERNAL` (`set_about.c:90`).
  - `hal_millis()`: `hal_esp32.c:111-114`, which returns `millis_raw()` = `esp_timer_get_time()/1000` truncated to `uint32_t` (`:68-70`). Used at `set_about.c:93`.
- **Engine** (`nos/`)
  - `nos_t`, the animation clock (`nos/nos.c:27`).
  - `nos_revealed()` (`nos.c:121`) and `nos_enter_t()` (`nos.c:129`), in `about_enter` (`set_about.c:108`, `:116`).
  - `nos_motion_probe()`. A no-op on device (`nos/nos.h:332`). It is live only in `GFX_COST_MODEL` builds (`set_about.c:124-158`).
- **Build constants.** `NOS_VERSION` (`nos/nos.h:27`), `NOS_SOURCE_BUILD_ID` (generated `generated/nos_build_identity.h`, included at `set_about.c:28`), and `UI_NONE`.
- **Indirect, through the sampler.** The rail value is written by `hal_esp32_sample_power()` (see Background). That reads GPIO4 on ADC1 channel 3 with the board divider (`firmware/main/pins.h:31-32` for 1.43C, `:65-66` for 1.43-B, divider 3). It also checks the USB-serial-JTAG connection (`hal_esp32.c:323`). It reads NVS key `bat_cal` once (`hal_esp32.c:318`), which affects the percent only, not the rail (`:354`, `:361`).

## Writes

- **Persistent: none.** There is no NVS, SD or prefs write. `set_about.c` has no `hal_store_*`, `prefs_*`, `svc_*` or SD call.
- **RAM only**
  - `static A` (`set_about.c:60-69`): the ui_list, the row start offsets, the version/PSRAM/SRAM strings, two `amt` morph records (`:59`), `next_upd` and `inited`.
  - `s_band_prev` and `s_band_lv` (`:277-278`).
  - Scroll state in `A.L`, through `ui_list_init`, `ui_list_set_n`, `ui_list_tick` and `ui_list_input` (`:112-115`, `:140`, `:340`).
- **Engine side effect.** `ui_fblur_tick(&A, ...)` (`set_about.c:304`) keys its cache on `&A`. It may allocate blur buffers with `hal_big_alloc` (`nos/ui_blur.c:463-476`). Their lifetime is owned by the engine (`nos/ui.h:243`).
- **Sim and bench only.** `nos_motion_probe` calls (`set_about.c:124-158`).

## Background behaviour

About has no background work. Its code runs only when the engine ticks and draws the top screen.

- **Screen on.** `nos_frame_run` calls `tick_screens()` (`nos/nos.c:2101-2110`, called at `:2347`), which calls `about_tick` for the top screen each frame. Frames run on the UI loop on core 0 (`firmware/main/nos_main.c:3`), at the presentation cap `nos_prefs.fps_cap`. The default is 60 (`nos/prefs.c:161`, `nos/frame_cap.h:5`, applied at `nos_main.c:2132`; the nominal target is `NOS_PRESENT_FPS` 60 at `nos/render_config.h:13`).
  - Values refresh when `nos_t >= A.next_upd` (`set_about.c:141`), set to `nos_t + 1.0 s` on each refresh (`:121`). `nos_t` advances only in frames (`nos/nos.c:2309`), so this is one second of on-screen time.
  - Uptime changes every second, so the uptime row starts an 8-frame morph every second (`set_about.c:71-78`, `MORPH_F` `:50`, `:259-270`). The glint adds 1.2 s of logo repaint every 5 s (`:154-156`, `:179-184`). Cost is not measured.
- **Screen off, no AOD.** The OFF branch of the loop (`firmware/main/nos_main.c:2091-2103`) does not call `nos_frame_run`, so `tick_screens` and `about_tick` stop and `nos_t` stops. The loop still calls `nos_bg_tick(1.0f)` at 1 Hz (`nos_main.c:2094`). That is `services_tick` in `nos/apps/registry.c:62-66`, which runs `svc_tick` and `background_tick`. Neither is About-specific. About stays on the stack and resumes on wake. Values refresh on the first tick after wake (`set_about.c:141`).
- **AOD or sensor idle.** `go_state()` calls `nos_home_now()` for `PWR_AOD` and `PWR_SENSOR` (`firmware/main/nos_main.c:705`; `nos/nos.c:429-437`). That pops About, and `leave` is 0 (`set_about.c:345`), so nothing runs on exit. AOD then runs the home face at 1 Hz (`nos_main.c:2114-2117`).
- **Popped.** No tick, and `static A` stays in RAM until reboot. A new entry calls `about_enter`, which resets the list when not revealed and restarts the logo build (`set_about.c:106-122`).
- **Who produces the rail value.** The `board-sense` task (`firmware/main/board.c:967`, created at `:1036`, priority 2, 4096-byte stack from `nos_mem_alloc` at `:1034`, `tskNO_AFFINITY`) loops about every 20 ms (`:1021-1025`). It calls `hal_esp32_sample_power()` once per second (`:1021`), and once at boot (`:1073`). The sampler is `hal_esp32.c:316-368` and takes 8 ADC reads per sample (`:327-334`). It runs with the screen off too. It also emits `SP_RAIL` to the sports journal (`hal_esp32.c:369-370`), which is written to SD (see [[Power and battery]]).

## Persistence

None. About persists nothing.

- Uptime counts from boot (`hal_esp32.c:68-70`) and resets at every reboot.
- The version and build ID come from the flashed image and are not stored by About.
- The rail sample lives in `s_power` in RAM (`hal_esp32.c:287-289`) and is not saved. The NVS key `bat_cal` is read by the sampler (`:318`), not by About.
- No SD path is touched.

## Navigation

- **Opened from**
  - Settings > "About" row (`nos/apps/set_settings.c:485`). Its action is `nos_push(&scr_about, TX_PUSH)` (`:1858`). Settings is the launcher tile "Settings" (`nos/apps/registry.c:36`).
  - Serial `open about`. The name lookup is `screen_by_name` (`firmware/main/nos_main.c:924-937`, entry at `:931`). The command at `:1558` also wakes the panel with `go_state(PWR_ON)`.
  - Simulator `B_PUSH` demo, which pushes Settings then About (`sim/nos_sim.c:1869`).
  - There is no launcher tile. `registry.c:35-36` lists only Diagnostics and Settings in that region, and About is not in the list.
- **Exits**
  - Rightward drag from anywhere on the screen pops About. About has no `SCR_HDRAG` flag (`set_about.c:345` flags 0), so the edge restriction does not apply (`nos/nos.c:848-856`).
  - A short side-button press calls `nos_home()` (`nos/nos.c:952`). About's input returns 0 for `IN_BTN` (`nos/ui.c:682-683`), so the engine handles it.
  - A long press opens the power sheet (`nos/nos.c:953-956`).
  - AOD or sensor sleep collapses the stack to home (`nos_main.c:705`).
- **Opens**: nothing. No row, tap or key pushes another screen.

## Gaps

1. **[medium] The frame-rate text is a literal and disagrees with the live cap.**
   - Evidence: `nos/apps/set_about.c:251` prints "466 x 466 AMOLED, 30 fps". The live cap is `nos_prefs.fps_cap`, default `NOS_FRAME_CAP_DEFAULT` 60 (`nos/prefs.c:161`; `nos/frame_cap.h:5`), applied at `firmware/main/nos_main.c:2132`. Developer Tools sets it (`nos/apps/set_developer.c:89-100`). `nos/prefs.h:43` lists 24, 30, 40, 48, 60 and 0 (unlimited).
   - Detail: nothing reads `fps_cap` on this page.
   - User impact: on default settings the page says 30 fps while the presentation target is 60.

2. **[medium] The battery rail ignores sample validity and can show a stale voltage.**
   - Evidence: `set_about.c:100` uses `pw.rail_mv > 0` only. `hal_power_get` marks `sample_valid=0` after 3000 ms but leaves `rail_mv` as it was (`hal_esp32.c:385-387`). The sampler copies the previous `rail_mv` when a sample fails (`:319`), and assigns a new one only when `sample_valid` is set (`:346`). `hal_battery_mv()` returns 0 when `!sample_valid` (`:389`).
   - Detail: a good reading followed by a failed one leaves the last voltage on the page with no marker. Failure cases include fewer than 5 good ADC reads of 8 (`:336`) and the board-sense task failing to start, which logs "base sampling unavailable" (`board.c:1037-1040`). In the second case only the boot sample at `board.c:1073` ever ran.
   - User impact: the page can show a voltage that is not current, while the HAL reports the rail as unknown.

3. **[low] The rail is a raw sensed voltage, and the page gives no charging or percent context.**
   - Evidence: `core/hal.h:34-36` calls it "Evidence, not a fuel gauge" and describes `external_power` and `battery_valid`. About uses neither (`set_about.c:98-100`). The row is labelled "BATTERY RAIL" (`:208`).
   - User impact: on USB the page shows about 4.1 V with no sign that the board is charging. The percent lives on the Battery screen (see [[Power and battery]]).

4. **[low] The board name is hard-coded and not read from `WATCH_BOARD_NAME`.**
   - Evidence: literals at `set_about.c:246-247`. `WATCH_BOARD_NAME` is defined at `firmware/main/pins.h:11` (1.43C) and `:36` (1.43-B).
   - User impact: correct on the 1.43-B build covered by this card. The same source built for `CONFIG_WATCH_BOARD_143C` would still say 1.43-B.

5. **[low] No hardware identity.**
   - Evidence: the BLE address is printed only at boot (`firmware/main/hal_esp32.c:1494-1496`). The serial READY line prints the firmware version and board name (`firmware/main/nos_main.c:1961`). The page shows no serial, MAC or BLE address.
   - User impact: support questions need a serial session.

6. **[low] Storage and flash rows are missing although the getters exist.**
   - Evidence: `hal_storage_get` (`core/hal.h:185-190`, `firmware/main/board.c:534`) and `hal_flash_info_get` (`core/hal.h:194-199`, `firmware/main/board.c:510-533`, running partition read at `:521`). Settings > Storage uses them (`nos/apps/set_settings.c:652`; `nos/apps/set_modules.c:215`). About calls neither.
   - User impact: no SD capacity, flash size or app slot size on the About page.

7. **[low] No firmware slot or update information.**
   - Evidence: the OTA API exists (`firmware/main/ota.h:18-47`), but no NOS screen calls it (grep of `nos/` finds no `ota_` call). The version row is `NOS_VERSION` (`nos/nos.h:27`). The ESP-IDF app description version is printed on the serial READY line (`nos_main.c:1961`), not on this page.
   - User impact: the page cannot show the running image, OTA state, or an update option.

8. **[low] No reset reason.**
   - Evidence: the reset reason is recorded in the diagnostics log (`nos/diag_log.c:265`), not shown on About.
   - User impact: after an unexpected restart the user sees a smaller uptime and no cause.

9. **[low] Uptime wraps after about 49.7 days.**
   - Evidence: `hal_millis()` is `esp_timer_get_time()/1000` truncated to `uint32_t` (`hal_esp32.c:68-70`). `set_about.c:93` divides it by 1000 in a `uint32_t`.
   - User impact: after the wrap the uptime restarts at zero. Rare on a watch, but wrong.

10. **[low] The page keeps repainting while open.**
    - Evidence: the uptime row morphs for 8 frames every second (`set_about.c:71-78`, `:50`, `:93-97`, `:141`). The logo glint runs for 1.2 s every 5 s (`:154-156`, `:179-184`).
    - User impact: a page that should be static costs panel and CPU time. Not measured on hardware.

11. **[low] Font credits show only the licence name.**
    - Evidence: `set_about.c:252-255`. The licence texts exist in `third_party/fonts/spacegrotesk/OFL.txt` and `third_party/fonts/spacemono/OFL.txt`, but not on the watch. The page has no copyright line.

12. **[low] No legal, regulatory or copy actions.**
    - Evidence: tap is consumed only to stop a fling (`set_about.c:338-341`). There is no open-source notices page and no copy of the build ID.
    - User impact: a modern About page normally offers legal notices and a copyable build or device string.

13. **[low] Doc drift.**
    - Evidence: `docs/audits/2026-10-21/report.txt:8653-8654` says the version is "hardcoded 1.0.0" and that About uses `__DATE__`. The code now uses `NOS_VERSION` "0.5.0-alpha" (`nos/nos.h:27`) and the build ID, with no `__DATE__` (`set_about.c:119`). `docs/os-map/systems/Settings and developer.md` (About section and gap 15) cites `set_about.c:246-251` for the literals. The literal rows now run to `set_about.c:255`, and its "default cap is 60" line is correct (`nos/frame_cap.h:5`).
