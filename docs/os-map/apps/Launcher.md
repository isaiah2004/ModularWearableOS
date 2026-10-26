---
title: "Launcher"
id: sys-launcher
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Launcher

Source: `nos/apps/sys_launcher.c` (1022 lines), `nos/apps/sys_launcher_page.h` (15 lines). App list and open paths: `nos/apps/registry.c`. Read-only source review of the 1.43-B build. Nothing was built, flashed or run on a board. Evidence is `repo-relative/path:line`.

## Summary

The Launcher is the NOS app list: a 28-row dial of icons and names on the round glass. On device the only user-facing path is the **Apps page of HOME**, not a standalone screen. HOME embeds it as pager page 1, to the right of the face (`nos/apps/sys_home.c:445`, `:604-609`, `:903`; slot model `:48-53`, `:289`). The standalone `scr_launcher` (`nos/apps/sys_launcher.c:994`) is reachable on device only through the serial command `open launcher` (`firmware/main/nos_main.c:924-933`, `:1558-1559`) and in the simulator (`sim/nos_sim.c:1911`).

The data is static. The list is a `const` table of 28 entries (`nos/apps/registry.c:8-36`). The screen calls no `svc_*` getter, reads no prefs key directly, and writes nothing to NVS, SD, prefs or services. Its state is RAM (scroll, focus, press, last app) plus PSRAM sprite caches that are rebuilt after reboot.

Hardware it needs: PSRAM for sprites and tables (`firmware/main/hal_esp32.c:1595-1598`, `MALLOC_CAP_SPIRAM`). Without PSRAM the rows are drawn live as vector shapes (`nos/apps/sys_launcher.c:855-868`). Touch comes from the shell. Vibration and UI sound are optional and reached only through `nos_feedback` (`nos/nos.c:557-573`). The list needs no sensor, radio, SD card or strap module, but it shows rows for hardware that may be absent (see Gaps).

Docs versus code, checked:

- `docs/NOS_DESIGN.md:123` gives "on face -> Launcher (TX_IRIS from the button side)". The code cuts to the Apps page inside HOME (`nos/apps/sys_home.c:1151-1155` calls `go_page(1)`, which calls `cut_to`, `nos/apps/sys_home.c:413-419`). No iris is pushed.
- `docs/NOS_DESIGN.md:126` (tap opens the app with TX_IRIS from the icon) matches the code (`nos/apps/sys_launcher.c:959-971` to `nos/apps/registry.c:55-60`).
- `nos/apps/registry.c:40-41` says the last app is recorded when opened "from anywhere (launcher, widget, notification, CC)". The code records it only in `nos_open()` and only for screens in `nos_app_list` (`nos/apps/registry.c:47`). Control centre (`nos/apps/sys_home.c:1090` uses `nos_pull`) and face picker (`nos/apps/sys_home.c:1012`, `:1140`, `nos_push`) never update it.
- `nos/apps/sys_launcher.c:1-27` says each icon and name is "baked once". Trims discard sprites: `trim_rows(1)` on card prepare (`nos/apps/sys_launcher.c:226`, `nos/psram.c:149`), and `trim_rows(0)` discards blur variants on Settings open (`nos/apps/set_settings.c:1234-1236`). Rows then re-bake.

## Features

- **28-row rotary dial.** The focus row has the full-white name and largest icon disc. Rows further out dim in 64 ink steps and slide along one bezel curve (`nos/apps/sys_launcher.c:1-16`, `:597-635`, curve table `:85`, `:322`).
- **Momentum scroll.** Decay constant `TAU_S` 0.4995 s, the 0.998/ms iOS-style decay (`nos/apps/sys_launcher.c:60`, `:507-525`). A fling lands on the nearest row (`snap_row`, `:473`, `release`, `:486-505`). A slow release becomes a critically damped spring (`spring_to`, `:475-484`). Beyond the ends a rubber band applies (`:949-951`).
- **Detent haptic.** Each time the focus row changes, `nos_feedback(FB_TICK)` fires (`nos/apps/sys_launcher.c:531-534`). Ticks are silent by design (`nos/nos.c:571-572`). The pulse is `HAPTIC_TICK` when `prefs.haptics` is set (`nos/nos.c:557-573`).
- **Press feedback.** A held row lights after 90 ms of stillness within 3 px. It stays lit until release or until travel exceeds 8 px. After release it stays lit for about 90 to 100 ms (`nos/apps/sys_launcher.c:113-136`).
- **Tap opens the app through an iris from its icon.** The icon position and radius seed the iris (`nos/apps/sys_launcher.c:959-971`, `nos/apps/registry.c:55-60`). A touch that begins while the list moves faster than `CATCH_V` (250 px/s, `nos/apps/sys_launcher.c:61`) only stops it, and its tap is swallowed (`:935`, `:960`, `:963`). On a slower glide the touch is a tap on the row under the finger.
- **Horizontal drags go back to HOME.** A horizontal `IN_DRAG_START` releases the list and returns `0` (`nos/apps/sys_launcher.c:943`).
- **Fling blur.** Eight directional pre-blurred variants (`nos/apps/sys_launcher.c:54-64`), chosen by `ui_fblur_level` (`:580`) and the motion policy (`nos_mblur`, `:572-580`).
- **Name fade at the glass edge.** A name too long for its chord fades out in three ink steps over 36 px rather than pulling its row (`nos/apps/sys_launcher.c:6`, `:791-840`).
- **Title.** "APPS" is drawn at the top and lifts with scroll (`nos/apps/sys_launcher.c:848`).
- **Build-in.** On the standalone screen rows arrive outward from focus, two frames per row, sliding 40 px (`nos/apps/sys_launcher.c:554-560`, `:624-630`). On the embedded page the build-in is skipped and the list shows as already built (`:1000-1001`).
- **Double press reopens the last app,** handled by HOME (`nos/apps/sys_home.c:1157-1161`). The standalone branch with the same behaviour (`nos/apps/sys_launcher.c:976-988`) never runs when embedded (see Gaps).

## Reads

**Service getters.** None. `grep svc_` in `nos/apps/sys_launcher.c` returns nothing. The screen includes `svc.h` but calls no getter.

**Prefs keys.** None read directly (`grep prefs` in `nos/apps/sys_launcher.c` finds only the include, `:36`). Indirectly, through `nos_feedback` (`nos/nos.c:557-573`):
- `nos_prefs.haptics` (`nos/prefs.h:9`), for `HAPTIC_TICK` on focus change (`nos/apps/sys_launcher.c:534`) and `HAPTIC_CLICK` on open (`nos/apps/registry.c:51`).
- `nos_prefs.sound` (`nos/prefs.h:11`), for `SFX_OPEN` on open (not for ticks, `nos/nos.c:571-572`).

**Registry and engine state.**
- `nos_app_list[i].name`, `.icon`, `.scr` and `nos_app_count` (`nos/apps/registry.c:8-38`).
- `nos_last_app()` (`nos/apps/registry.c:43`).
- `nos_t`, `nos_enter_t()` and `nos_revealed()` (`nos/nos.c:121`, `:129`; `nos/apps/sys_launcher.c:541`, `:545`, `:1000`).
- `nos_touch_travel()` for press travel (`nos/apps/sys_launcher.c:128`, `:135`; `nos/nos.c:618`).
- `nos_mblur` and `NOS_HELD_SHARP` (`nos/apps/sys_launcher.c:572`, `:580`; `nos/nos.c:34`; `nos/nos.h:113-115`).
- `ui_fblur_level` and `ui_fblur_coverage_claim` (`nos/apps/sys_launcher.c:580`, `:877`, `:909`, `:914`; `nos/ui_blur.c:290`, `:333`).
- `nos_iris_x`, `nos_iris_y` (written, see Writes).

**Hardware and allocator.**
- `hal_micros()` for the prewarm time budget (`nos/apps/sys_launcher.c:447`, `:460`).
- `hal_big_alloc` and `hal_big_free` for sprite and table memory (`nos/apps/sys_launcher.c:213-214`, `:235-238`, `:298`; `firmware/main/hal_esp32.c:1595-1598`).
- `nos_mem_available()` for room checks under `NOS_PSRAM_TRACKING` (`nos/apps/sys_launcher.c:250-252`; `nos/psram.c:96`). It keeps a 512 KiB reserve (`nos/apps/sys_launcher.c:90`).
- `hal_haptic` and `hal_sfx`, reached only through `nos_feedback` (`nos/hal_ext.h`; `firmware/main/ext/ext_audio.c:358`).

**Assets.**
- Native menu ROM: `native_menu_assets_compatible`, `native_menu_asset`, `native_menu_sprite`, `native_menu_launcher_palette`, `native_menu_launcher_curve`, `native_menu_launcher_matches` (`nos/apps/sys_launcher.c:145-186`; `nos/native_menu_assets.c:325015-325058`).
- The baked table of 28 names and icon ids (`nos/native_menu_assets.c:325003-325004`).
- Fonts `UI_F_TITLE`, `UI_F_BODY`, `UI_F_LABEL` (`nos/apps/sys_launcher.c:151-155`, `:346`, `:822`).
- Icon glyphs `dot_icon()` (`nos/apps/sys_launcher.c:364`, `:863`).

## Writes

**Navigation.**
- `nos_open_icon` and therefore `nos_open` (`nos/apps/sys_launcher.c:966-969`) push the app with `TX_IRIS` (`nos/apps/registry.c:45-60`).
- `nos_iris_seed` and `nos_iris_seed_r` (`nos/apps/registry.c:58-59`).
- Standalone double press (`nos/apps/sys_launcher.c:976-988`) calls `nos_retarget` or sets `nos_iris_x`, `nos_iris_y` (`:983-984`) and calls `nos_replace(last, TX_IRIS)` (`:985`). Those are engine globals (`nos/nos.c:327`, `:361`).

**Last app.** `s_last_app` (`nos/apps/registry.c:42`), set in `nos_open` only for list members (`nos/apps/registry.c:47`).

**Feedback.** `nos_feedback(FB_TICK)` on focus change (`nos/apps/sys_launcher.c:534`). `FB_OPEN` on open, via `nos_open` (`nos/apps/registry.c:51`).

**PSRAM.**
- One block `L` allocated lazily (`nos/apps/sys_launcher.c:296-299`). It holds a 65 x 256 grey-to-RGB565 LUT (33,280 B), a press LUT (512 B), a 320 x 64 RGB565 scratch canvas (40,960 B), a curve table (1,300 B), and 32 row descriptors. The LUT, press LUT, blur sums, scratch and curve total 76,692 B, before the 32 row descriptors.
- Per row: three icon sprites of 76 x 76 (`ISPR`, `:57`), a name sprite (up to 320 wide, `:348`), and eight icon and name blur variants (`:178-185`, `:381-421`).
- Freed by `trim_rows` (`:196-224`) and `sys_launcher_cache_reset` (`:229-245`).
- ROM rows own no PSRAM. Their pixels are borrowed from the ROM arena and never freed (`:205`, `:234`).

**Static RAM.** Scroll `S` (`nos/apps/sys_launcher.c:99-110`), press `P` (`:115`), bake job `J` (`:96`), bake counters (`:87-88`).

**Not written:** NVS, SD, prefs, services, sensors.

**Simulator only.** `nos_motion_probe` calls are compiled out on device (`nos/nos.h:330-332`).

## Background behaviour

**The Launcher owns no task, timer or ISR.** Its code runs only from the UI loop in `app_main` (`firmware/main/nos_main.c:1899`), on the ESP-IDF main task pinned to CPU0 with an 8192-byte stack (`firmware/sdkconfig:1706-1710`). `firmware/sdkconfig` does not override the main task priority.

**Who ticks it.**
- While the Apps page is on glass, `home_tick` calls `sys_launcher_page_tick` every engine frame (`nos/apps/sys_home.c:602-609`). The gate is `!nos_aod && !H.covered && nos_top()==&scr_home` and the page within `PAGE_W`.
- When the standalone screen is the top screen, `tick_screens` calls its `l_tick` (`nos/nos.c:2101-2110`).
- Engine frame pace: present target 60 Hz (`nos/render_config.h:13`, `firmware/main/nos_main.c:2135`; the `fps_cap` pref can override). Motion is normalised to a 40 Hz logical frame (`nos/render_config.h:8`, `nos/apps/sys_launcher.c:510`, `:578`).

**Sprite baking (the only background work).**
- From the Launcher's own tick, whenever its page is visible (`nos/apps/sys_launcher.c:591`).
- From HOME's idle path (`nos/apps/sys_home.c:638-646`). This runs only when the Apps page is off, not in AOD, not dragging or settling, and the face is not interactive. It runs on odd frames where `nos_frame % 3 == 0`, about one engine frame in six. The other arms call `sys_notifs_prewarm` and `set_settings_prewarm` (`nos/apps/sys_home.c:641-645`).
- Budget per call: 750 µs (`nos/apps/sys_launcher.c:93`), at most 16 steps (`:94`). Each step bakes at most 8 rows (`:95`). The budget is checked after each step (`:452-468`).
- Row choice (`bake_next`, `nos/apps/sys_launcher.c:422-436`): rows within three pitches of the current or predicted scroll first, then rows still lacking a sharp sprite, then nearest. Rows already published, including ROM-backed rows (`:186`), are skipped (`:429`). ROM rows are adopted first (`:456`).

**Closed.** Nothing from the Launcher runs. Its PSRAM caches stay resident until a trim. Trims happen on Settings open (`nos/apps/set_settings.c:1236`), card prepare (`nos/psram.c:138-151`, which `nos/faces/orrery_card.c:1870` triggers), the `swipe_cold` diagnostic (`firmware/main/nos_main.c:1363-1371`) and the ROM toggle (`firmware/main/nos_main.c:1336-1345`). `nos_mem_idle` (`nos/nos.c:2344`) does not touch the Launcher.

**Screen off (PWR_OFF).** The engine does not run `nos_frame_run`. The loop sleeps 20 ms per pass and calls `nos_bg_tick(1.0f)` once a second (`firmware/main/nos_main.c:2091-2100`). The Launcher gets no tick and no bake.

**AOD.** The loop runs `nos_frame_run` once a second (`firmware/main/nos_main.c:2106-2121`). `apps_visible` requires `!nos_aod` (`nos/apps/sys_home.c:602`), and baking requires `!nos_aod` (`nos/apps/sys_home.c:638`). The Launcher is not ticked and not baked.

**Services, which run regardless of the list.** `services_tick` (`nos/apps/registry.c:62-65`) calls `svc_tick` and `background_tick`. It runs every engine frame through `nos_bg_tick` (`nos/nos.c:2341`), and once a second with the screen off (`firmware/main/nos_main.c:2094`). See [[Services hub]] and [[Engine and scheduling]]. Power effects are in [[Power and battery]].

## Persistence

**Nothing is persisted by the Launcher.** There is no NVS key, no SD path and no prefs field it owns (`grep prefs` in `nos/apps/sys_launcher.c` returns only the include).

What survives within a boot, in RAM only:
- Scroll position and focus. `S.x` is kept across openings and snapped to the nearest row on each enter (`nos/apps/sys_launcher.c:541`). It is not saved across reboot.
- Last app (`nos/apps/registry.c:42`). Not saved across reboot.

What is rebuilt after reboot:
- PSRAM sprite caches (above).
- The native-art arena. It is decoded from the flash blob at boot (`nos/native_menu_assets.c:321771-321801`, `NATIVE_UNPACKED_BYTES` 1,499,985 at `:140644`). It is shared with the Settings screen, uses pixel-hash verification, and is kept for the session (`native_unpacked=arena`, `:321801`). On failure the Launcher falls back to runtime baking (`nos/apps/sys_launcher.c:855-858`).

Storage and persistence context: [[Storage and persistence]].

## Navigation

**Opened from**
- HOME, Apps page (pager page 1, `LAST_SLOT` 1, `nos/apps/sys_home.c:52-53`). Reached by:
  - the face side button (`nos/apps/sys_home.c:1151-1155`, `go_page(1)`),
  - an air swipe (`nos/apps/sys_home.c:1163-1170`),
  - a horizontal pager drag (`nos/apps/sys_home.c:1030-1032`, a touch that begins at `H.x == PAGE_W`).
- Serial `open launcher` (`firmware/main/nos_main.c:1558-1559`). This opens the standalone screen with a `TX_IRIS` from the screen centre.
- Simulator button `B_APPS` (`sim/nos_sim.c:1911`).

**Opens**
- Any of the 28 apps by tap (`nos/apps/sys_launcher.c:959-971`, `nos/apps/registry.c:55-60`). The registry order is:
  - Activity, Sports, Energy, Jobs, Chat, Battery history, Sensor logs, Wrist tuning, Quick report, Heart rate, Breathe, Weather, Music, Timer, Stopwatch, Alarms, World clock, Compass, IMU, Barometer, Navigate, Cellular, Recorder, Find phone, Torch, Developer tools, Diagnostics, Settings (`nos/apps/registry.c:9-36`).
- The last app, by double press on HOME (`nos/apps/sys_home.c:1157-1161`).

**Exits**
- HOME button on the Apps page returns to the face (`nos/apps/sys_home.c:1151-1155`, `go_face`).
- A horizontal drag returns to the HOME pager (`nos/apps/sys_launcher.c:943`).

**Related screens.** [[Shell and faces]] (HOME, pager, face). The app screens are in their own notes, grouped under [[Health and activity]], [[Sports and workouts]] and [[Connectivity and voice]].

## Gaps

- **Hardware-gated rows appear with no fit indication (medium).** Evidence: `nos/apps/registry.c:13` (Chat), `:29` (Navigate), `:30` (Cellular), `:31` (Recorder). `nos/apps/sys_launcher.c` makes zero `hal_ext_present` calls. Each target screen checks presence itself: `nos/apps/con_cell.c:96` and `:629`, `nos/apps/out_nav.c:537`, and Chat shows "Voice chat needs the C board mic and speaker" (`nos/apps/con_chat.c:131-132`). The view is available only when `CONFIG_WATCH_BOARD_143C` is set (`firmware/main/voice_chat.c:113-114`). Impact: on the 1.43-B build, where CLAUDE.md records no built-in audio, the user can open Chat and land on a page that says it needs the C board's mic and speaker. The list never says so.
- **No list control: order, visibility and selection are fixed (medium).** Evidence: `nos/apps/registry.c:8-38` is a `const` table. No launcher-related key exists in `nos/prefs.h`. `napps()` counts every entry (`nos/apps/sys_launcher.c:141`). Impact: no reorder, favourites, hide, rename, search, alphabetical sort or recents. 28 rows must be scrolled past to reach Settings.
- **No live state on rows (medium).** Evidence: rows draw only an icon disc and a name (`nos/apps/sys_launcher.c:863-866`, `:904`). The file has zero `sys_rec_*` and zero `svc_*` references. The recording chip exists only on HOME (`nos/apps/nos_apps.h:23-25`). Impact: no badge for an active track, memo, IMU capture or workout, no unread or step count, and no state at all. Related: [[Health and activity]], [[Sports and workouts]], [[Services hub]].
- **No long-press on rows (medium).** Evidence: `l_input` returns 0 for `IN_LONG` (`nos/apps/sys_launcher.c:989-990`). The page facade forwards it (`:1011-1016` only filters BTN and AIR). HOME's `IN_LONG` acts only on the face (`nos/apps/sys_home.c:1135-1144`). Impact: no context menu on an app (pin, hide, info), which is a common smartwatch expectation.
- **Last app is RAM-only and narrow (medium).** Evidence: `nos/apps/registry.c:42` is static. `nos/apps/registry.c:47` records only list members. `nos/apps/sys_home.c:1159-1161` returns 0 when unset. The comment at `nos/apps/registry.c:40-41` overstates the scope. Impact: after a reboot, double press does nothing until an app is opened. Control centre, notifications and the face picker never become "last", despite the comment.
- **Developer tools and Diagnostics are always listed (low).** Evidence: `nos/apps/registry.c:34-35`. The Developer screen is defined at `nos/apps/set_developer.c:143`. No gate exists in the registry or the launcher. Impact: debug screens sit among everyday apps for every user.
- **Standalone-only code is effectively dead on device (low).** Evidence: `nos/apps/sys_launcher.c:1013-1014` filters `IN_BTN_DOUBLE` and `IN_BTN` from the embedded page, so the standalone branch at `:976-988` never runs there. The standalone screen is reachable only via serial or the simulator (`firmware/main/nos_main.c:1558-1559`, `sim/nos_sim.c:1911`). Impact: maintenance burden. The double-press feature works only through HOME (`nos/apps/sys_home.c:1157-1161`).
- **Scroll and focus are not kept across reboot (low).** Evidence: `nos/apps/sys_launcher.c:99-110`, `:541`. Impact: after reboot the list starts at row 0.
- **Native art is keyed to a generated name and icon table (low, maintainability).** Evidence: `nos/native_menu_assets.c:325003-325004`, matched in `:325020-325023` and `nos/apps/sys_launcher.c:166`. A rename, reorder or icon change in `nos/apps/registry.c` silently sends that row to runtime baking (`nos/apps/sys_launcher.c:855-858`). The 1,499,985-byte arena stays resident (`nos/native_menu_assets.c:321773`, `:321801`). Impact: extra PSRAM and CPU, not a visible failure.
- **Open sound may be silent on B (low, unverified).** Evidence: `nos/nos.c:557-573` plays `SFX_OPEN` on tap when `prefs.sound` is set. `firmware/main/ext/ext_audio.c:358-361` returns early unless the audio task and volume state exist. CLAUDE.md records no built-in audio on 1.43-B. Impact: haptic feedback is the reliable confirmation on B. Sound on B was not established from source.
- **Dead declaration (low).** Evidence: `nos/apps/sys_launcher.c:40` declares `sys_home_open_after`, which the file never uses. Impact: none beyond clutter.
- **"Baked once" overstates (low, documentation).** Evidence: header `nos/apps/sys_launcher.c:1-27`. Trims discard sprites (`:196-226`) and Settings open re-bakes blur variants (`nos/apps/set_settings.c:1234-1236`). Impact: after a Settings visit or a card prepare the first list frames can re-bake.
