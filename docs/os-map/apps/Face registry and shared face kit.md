---
title: "Face registry and shared face kit"
id: face-registry
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Face registry and shared face kit

Source: `nos/faces/faces.c` (840 lines), `nos/faces/face.h` (41), `nos/faces/face_kit.h` (216), `nos/faces/face_ext.h` (58). Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from source reading only, not from the device. The nine individual face renderers are out of scope here except where the registry dispatches to them.

## Summary

The registry is `nos/faces/faces.c`. It holds the compile-time table of the nine watch faces (`nos/faces/faces.c:13-17`). DOT at index 0 is the default because the prefs default is 0 (`nos/prefs.c:148`). It also owns the AOD burn-in drift (`faces.c:19-64`), the per-face extras (`nos_face_style`, `nos_face_input`, `nos_face_interactive`, `nos_face_press_rect`, `faces.c:814-840`), and the shared kit the faces draw with (`nos/faces/face_kit.h`, implemented in `faces.c:66-811`). The kit covers time and date text, label cross-fades, minute morphs, the wake-from-AOD morph, the unread dot, the battery pill, scaled digit fonts, roll and blink motion, and batched dot lists with a PSRAM cache.

The contract is in `nos/faces/face.h`. `face_ctx` is the per-frame input (`face.h:14-27`). `nos_face` is the vtable of name, enter, tick, draw and tap (`face.h:29-36`). `nos/faces/face_ext.h` declares the optional extras and the ORRERY tuning knobs.

The registry has no task, timer, ISR or service registration. Nothing under `nos/faces/` calls `xTask*`, `esp_timer` or any timer API. It runs only when a caller invokes it inside the UI loop: home (`nos/apps/sys_home.c:612-625`, `:900-953`), the face picker (`nos/apps/sys_faces.c:155`, `:284-286`), and the AOD and wake paths. The face data comes from `build_ctx()` in `nos/apps/sys_home.c:465-490`, the only producer of `face_ctx`. The picker shares it through `sys_home_ctx()` (`sys_home.c:493-497`, `sys_faces.c:136-137`). The registry's live direct calls are limited to `hal_millis`, `hal_big_alloc`, `hal_big_free` and the `nos_prefs.hour24` flag. `hal_get_datetime` appears only in the unused `face_aod_shift`. It calls no `svc_*` getter and makes no prefs write.

Where the docs and comments differ from the code:
- `face.h:7` and `face.h:40` say the burn-in drift comes from `face_aod_shift()`. Nothing calls it. The drift on device is `shift_for()`, reached through `fk_aod_update()` (`faces.c:57-64`). `face_aod_shift()` (`faces.c:28-32`) is an unused export.
- `face_kit.h:171-172` says `fk_aod_update` runs the SEGMENT warm-up. The code runs it only when the face is awake and fully built in (`faces.c:61`).
- `faces.c:1` says "best first". The order is simply the table order. Nothing ranks the faces.
- `docs/os-map/systems/Shell and faces.md` says home ticks at 30 fps. The constants are `NOS_FPS` 40 for motion and `NOS_PRESENT_FPS` 60 for presentation (`nos/render_config.h:8`, `:13`). The motion clock is what the kit counts frames against (`faces.c:70-77`, `nos/motion.h:25`).

## Features

- **Nine faces in table order:** DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE, ORRERY (`faces.c:15-16`). Each picker label is the face's `name` field (`face_dot.c:155`, `face_matrix.c:387`, `face_glyph.c:253`, `face_split.c:240`, `face_life.c:252`, `face_segment.c:623`, `face_inverted.c:176`, `face_tape.c:275`, `face_orrery.c:2555`).
- **12 h or 24 h time** from the one global `hour24` setting (`faces.c:79-100`, `nos/prefs.h`). The 12 h form drops the leading zero. Shared helpers build `HH`, `MM` and `H:MM` (`fk_hh`, `fk_mm`, `fk_time`).
- **AOD burn-in drift.** Each minute the drift is a fixed Lissajous-style step of up to 4 px in x and in y, taken from the clock's hour and minute (`faces.c:19-26`, `:57-64`). It is zero while awake (`faces.c:62`).
- **Wake from AOD.** A 6-frame counted morph (`WAKE_N`, `faces.c:455`). The AOD drift walks back home at 1 px per frame (`faces.c:490-504`). A gap of more than 500 ms without ticks marks the face stale, so morphs settle instead of replaying a change nobody saw (`faces.c:458-466`).
- **Build-in on appear or wake.** `enter` runs from 0 to 1 over `ENTER_S` = 0.40 s (`nos/apps/sys_home.c:55`, `:485`). The kit turns that into 12 frame steps (`face_kit.h:31-32`).
- **Unread dot.** 8 px at x = UI_CX (233), y = 24, fading in four perceptual steps. In AOD it is a 6 px grey dot (`faces.c:551-579`, `face_kit.h:122-133`).
- **Battery pill.** Shows "NN%" with a bolt while charging, when charging or at 20% or below. In AOD it shows only below 10% and not charging (`faces.c:582-599`).
- **Date label** "SUN 11" from the weekday and day (`faces.c:107-110`, `nos/ui.c:914-917`).
- **Motion helpers.** Label cross-fade (`faces.c:146-177`), minute morph (`faces.c:120-143`), a mechanical digit roll in three held steps over 5 frames (`faces.c:750-799`), a once-a-second blink envelope (`faces.c:801-811`).
- **Touch on complications** through press rectangles and tap targets (`faces.c:830-840`):
  - SPLIT rows: row 0 heart, row 1 activity, row 2 weather only when weather is valid (`face_split.c:218-238`, `:113`).
  - SEGMENT: the ring area outside r = 160 and the label band open Activity (`face_segment.c:603-621`).
  - INVERTED: the band at y 310-380, x 120-346, opens Activity (`face_inverted.c:165-170`).
  - DOT, MATRIX, GLYPH, LIFE and TAPE have no tap target (`face_dot.c:155`, `face_matrix.c:385`, `face_glyph.c:253`, `face_life.c:252`, `face_tape.c:275`).
- **Styles** (`faces.c:814-819`). INVERTED is `FACE_LIGHT | FACE_FULL_BLEED` (a white face). MATRIX, SEGMENT, GLYPH, LIFE and ORRERY are `FACE_FULL_BLEED`. Home and the picker use these to set the page-dot colour and the pill moat.
- **ORRERY owns touch** through `nos_face_input` (`faces.c:826-829`), and is excluded from press rectangles (`faces.c:838`). While home is on the face page, touches are offered to it first (`sys_home.c:1020`, `:1028`).
- **Dot lists** (`faces.c:298-350`). A face's dot set is evaluated once per frame into a PSRAM cache, then stamped per 16-row stripe. The raster path tests each stripe against bounding boxes.

## Reads

### Registry (`nos/faces/faces.c`, `face_kit.h`)

- `hal_get_datetime()`: `faces.c:30`, only inside `face_aod_shift()`, which has no callers. The live path reads the clock once per frame through `build_ctx` (`sys_home.c:468`). On device `hal_get_datetime` reads the ESP-IDF system clock through `gettimeofday` and `localtime_r` (`firmware/main/hal_esp32.c:116-146`).
- `hal_millis()`: `faces.c:460`, the wake stale and skip timing.
- `nos_prefs.hour24`: `faces.c:81`, `:89`. Also `face_matrix.c:188`, `:199`, `face_orrery.c:2021`.
- `nos_t` (frame clock): `faces.c:248-249`, the dot cache key.
- `nos_subview()`: `faces.c:794`, for the roll clip window.
- `nos_motion_probe()`: `faces.c:512`, sim-only. On device it is a no-op macro (`nos/nos.h:330-332`).
- `ui_weekday3()` and the UI text and label calls: `faces.c:105`, `:109` (`nos/ui.c:914-917`).

### Face context (`face.h:14-27`), filled by `build_ctx()` (`nos/apps/sys_home.c:465-490`)

- `svc_activity()`: `steps` and `goal` (`sys_home.c:469`, `:471-472`; `nos/svc.h:95-102`). Steps fall back to `svc_sys()->steps_today`.
- `svc_sys()`: `steps_today`, `battery_pct`, `charging` (`sys_home.c:470-475`; `nos/svc.h:176-185`). `battery_pct` falls back to `hal_battery_percent()` when zero (`sys_home.c:474`, `core/hal.h:32`).
- `nos_prefs.step_goal_100`: fallback goal (`sys_home.c:472`).
- `svc_heart()->bpm_last`: `sys_home.c:473`, `svc.h:106`. `bpm_last_epoch` (`svc.h:107`) is not read.
- `svc_notif_unread()`: `sys_home.c:476`, `svc.c:238-242`.
- `svc_weather()`: `valid`, `temp_c`, `cond` (`sys_home.c:477-480`; `svc.h:66-88`). The `stale`, `age_seconds` and `updated_epoch` fields are not read (`svc.h:69-70`).
- `svc_alarm_next()`: `next_alarm_min` (`sys_home.c:482`, `svc.c:768-800`). No face reads the result.
- `nos_aod`, `H.enter_t0`, `H.rec_raw`: the AOD flag, build-in clock and REC chip ramp (`sys_home.c:483-488`).
- `battery_valid` (`svc.h:180`) is stored by `svc_tick` (`svc.c:994`) and is not read into the context.

### Faces reading services directly (outside the registry)

- `svc_activity()`: `face_segment.c:261`, `face_split.c:77`, `face_inverted.c` (one call).
- `svc_heart()`: `face_split.c:76`.
- `nos_prefs.units_metric`: `face_split.c:81-82`.
- ORRERY: `hal_epoch_seconds()` (`face_orrery.c:1143`, `:1183`), `nos_top`, `nos_transitioning`, `nos_mem_available`, `hal_micros` (profiling only), `nos_feedback` (haptic, 18 calls, `face_orrery.c`).
- ORRERY card: `hal_sd_root()` (`orrery_card.c:776`, `firmware/main/hal_esp32.c:1592`), and the asset job calls `hal_asset_submit/poll/take/busy/cancel` (`orrery_card.c:649-786`, `hal_esp32.c:2500-2520`).

## Writes

- **RAM only, in the registry:** `s_dc_epoch` (`faces.c:35`, `:59`), the seconds clock `s_sk` (`faces.c:40-49`), `fk_aod_dx` and `fk_aod_dy` (`faces.c:34`, `:62-63`), the unread level `s_unread_k` (`faces.c:548`, `:556-559`), and the static ~4 KB sampling table `tx` in the scaled-glyph builder (`faces.c:639-641`).
- **PSRAM, allocated and kept:** the dot-list cache `s_dc`, about 37 KB (`faces.c:218-224`, `:242`). It is never freed. Each scaled digit's coverage mask is a PSRAM block (`faces.c:632`, `:669`). The `tx` sampling table is allocated once and shared by all scaled glyphs (`faces.c:640`).
- **No prefs, NVS or SD writes** from `nos/faces/`. Searching the folder for `hal_store`, `prefs_save`, `fopen` in write mode and `nos_prefs.* =` finds nothing (ORRERY opens its textures read-only, `firmware/main/hal_esp32.c:2431`).
- **The face index** is the only face-related persisted write, and it is in the picker, not the registry: `nos_prefs.face = (uint8_t)p` and `prefs_save()` (`nos/apps/sys_faces.c:373-375`).
- **Sim-only diagnostics** through `fk_probe` (`faces.c:507-513`). They compile out on device.

## Background behaviour

- **Registry:** nothing runs by itself. It has no task, timer or callback registration (none in `nos/faces/`). It is called from the UI loop when home or the picker is the top screen.
- **Screen on (PWR_ON):** the UI loop presents at `NOS_PRESENT_FPS` 60, capped by the `fps_cap` preference, default 60 (`nos/render_config.h:13`, `nos/frame_cap.h:5`, `nos/prefs.c:187`). Motion is counted in 40 Hz motion frames (`nos/render_config.h:8`, `faces.c:70-77`). Home ticks the face every frame (`sys_home.c:612-625`).
- **Picker:** ticks the two visible faces each frame (`sys_faces.c:150-156`, `:155`) and draws one or two subviews (`sys_faces.c:278-286`).
- **AOD or sensor idle (PWR_AOD, PWR_SENSOR):** the face is ticked and drawn once per second, after the entry ramp (`firmware/main/nos_main.c:2114-2118`). The AOD drift changes only when the minute changes (`faces.c:20-26`, `:57-64`).
- **Screen off (PWR_OFF):** no face tick or draw. The loop calls `nos_bg_tick` once a second (`firmware/main/nos_main.c:2091-2094`). The services that feed the context keep running at the cadences below.
- **Services feeding the context:** `svc_tick` runs from `nos_bg_tick` (`nos/svc.c:1303-1329`). The 1 s slow path runs `sys_tick` and `act_tick` (`svc.c:1315-1319`). Heart history is folded in once a minute (`svc.c:1322`, `:586-600`). Weather refreshes every 10 s (`svc.c:412-416`).
- **ORRERY asset task** (`asset-read`): created lazily on the first texture request, pinned to no core, priority 1, 6 KB stack taken from PSRAM (`firmware/main/hal_esp32.c:2455-2497`). It is woken by `hal_asset_submit` (`hal_esp32.c:2500-2505`). It runs only while a texture job is pending. Its results are adopted from the UI loop (`orrery_card.c:673-700`), including from the screen-off and AOD loop (`firmware/main/nos_main.c:2089`) and at shutdown (`nos_main.c:774`).
- **ORRERY cache trim:** after 5 s with no draw, `orrery_cache_idle` trims its scratch (`nos/faces/face_orrery.c:2574-2577`), called from `nos_mem_idle` (`nos/psram.c:157-163`).

## Persistence

- **The registry persists nothing.** The dot cache, the seconds clock, the wake and unread state, and the ORRERY lock, time, tilt and view are RAM only. ORRERY is locked and reset on AOD (`face_orrery.c` tick, `c->aod` branch).
- **Face index:** `nos_prefs.face`, byte 12 of the preference blob by declaration order (`nos/prefs.h:20`). It is written only by the picker (`sys_faces.c:373-375`).
  - Primary store: SD, `/sdcard/watch/settings.0.bin` and `settings.1.bin` (`nos/prefs_disk.c:32`, `firmware/main/hal_esp32.c:243`).
  - NVS mirror: key `nosprefs` (`firmware/main/hal_esp32.c:236-265`), used when the card is absent.
  - Debounce: 2 s after the last change, at most 10 s (`nos/prefs.c:381-383`).
- **`face_accent`** is persisted in the same blob (`nos/prefs.h:21`, `nos/prefs.c:34`) and nothing reads it.
- **ORRERY textures:** read from `/sdcard/sky/<name>.565` or `.rgba` (`orrery_card.c:776`, `hal_esp32.c:1592`). They are generated by the build into `build/sd/sky`, which is git-ignored (`tools/build_identity.py:53-79`).

## Navigation

The registry is not a screen. The face is home page 0 (`nos/apps/sys_home.c:3`, `:273-276`).

- **Opened from:**
  - Home page 0, long press: pushes `scr_faces_home` with `TX_NONE` (`sys_home.c:1140`). See [[Face picker]].
  - ORRERY, 3 s hold: `face_orrery.c:1255` calls `home_open_faces()` (`sys_home.c:1008-1014`). The hold threshold is `HOLD_PICK` 3.0 s (`face_orrery.c:55`).
  - Settings, "Watch face" row: pushes `scr_faces` with `TX_PUSH` (`nos/apps/set_settings.c:1852`).
  - Serial bench `open faces` (`firmware/main/nos_main.c:930`). Development only.
- **Opens, through face taps:** a press first fetches the press rectangle at touch-down (`sys_home.c:1063`, `faces.c:830-840`). The tap then calls `f->tap` (`sys_home.c:1117-1129`).
  - SPLIT: `scr_heart`, `scr_activity`, `scr_weather` (`face_split.c:218-238`).
  - SEGMENT: `scr_activity` (`face_segment.c:603-621`).
  - INVERTED: `scr_activity` (`face_inverted.c:165-170`).
- **Touch routing:** ORRERY takes touch first while home is on page 0 (`sys_home.c:1020`, `:1028`). A locked ORRERY answers a tap with the toast "HOLD 1.5 S TO UNLOCK" (`face_orrery.c:2548-2553`).

## Gaps

- **[medium] Heart value never expires.** `bpm_last` is replaced only by a newer good reading and is never zeroed (`nos/svc.c:602-605`, `:631`). SPLIT shows it with no age check (`nos/faces/face_split.c:76`) and turns it red at 120 bpm or above, or below 40 (`face_split.c:109`, `:128`). A reading taken before the watch came off the wrist stays on the face. The timestamp exists (`nos/svc.h:107`) but no face reads it. User impact: a stale high reading keeps the heart row red indefinitely.
- **[medium] Clock validity is not carried.** `hal_datetime` has no valid flag (`core/hal.h:9-16`). When no time source has ever set the clock, the board sets it to epoch 0 (`firmware/main/board.c:884-886`). Every face then draws a 1970 date from `fk_date` and `fk_time` (`faces.c:95-110`) with no marker. User impact: a watch that has never synced shows a wrong date and time, and nothing says so.
- **[medium] No per-face or complication customisation.** The table is compile-time (`faces.c:13-17`). The only stored choice is the index (`prefs.h:20`). `face_accent` is unused (`prefs.h:21`). Users cannot pick complications (heart, steps, weather, next alarm), add or reorder faces, or set an accent. These are expected on a modern watch.
- **[low] Weather shown without staleness.** `sys_home.c:477-480` copies `valid`, `temp_c` and `cond` only. `stale`, `age_seconds` and `updated_epoch` are not read (`svc.h:69-70`). SPLIT's weather row (`face_split.c:80-81`, `:113`) therefore shows an old temperature as current, with no marker.
- **[low] Battery estimate treated as fact.** `sys_home.c:474` uses `battery_pct` and ignores `battery_valid` (`svc.h:180`). The HAL marks the value invalid on external power and on uncertain or stale samples (`core/hal.h:34-36`). The 20% pill (`faces.c:585`), MATRIX (`face_matrix.c:278`) and SPLIT battery (`face_split.c:78`) all show it as a plain percentage.
- **[low] Heart row is permanently "--" on a board without the module, with no reason shown.** The heart source is an optional MAX30102 probed at I2C `0x57` (`firmware/main/ext/ext_hr.c:314-324`). Without it the history is empty (`ext_hr.c:384-386`), so `bpm_last` stays 0 and SPLIT prints "--" (`face_split.c:127`). The B pin map has no heart pins (`firmware/main/pins.h`).
- **[low] Next-alarm and weather-condition fields are computed but unused.** `next_alarm_min` (`sys_home.c:482`, `face.h:23`) and `wx_cond` (`sys_home.c:480`, `face.h:22`) are filled every frame. No file under `nos/faces/` reads either. No face shows the next alarm or a weather icon.
- **[low] Five of nine faces have no tap target.** DOT, MATRIX, GLYPH, LIFE and TAPE return no tap screen (`face_dot.c:155`, `face_matrix.c:385`, `face_glyph.c:253`, `face_life.c:252`, `face_tape.c:275`). Tapping the time or date does nothing.
- **[low] Dot-list batching is lost for the session after one failed allocation.** `dc_find` tries the PSRAM cache exactly once (`faces.c:238-244`, `static int tried`). After a failure it returns 0 for the rest of the boot. There is no retry, log or diagnostic. The design comment says the batching avoids about 30 callback runs per dot per frame (`faces.c:205-211`). User impact: a silent frame-cost regression, not a crash.
- **[low] ORRERY needs SD textures that are not in git.** The textures come from the git-ignored `build/sd/sky` (`tools/build_identity.py:53-79`). Without them the planets draw as flat tints (`orrery_card.c:1000`). If the card cannot allocate at all, the face falls back to the ROM clock (`face_orrery.c:2193`). Neither case shows a message.
- **[low] Date text is English-only and fixed.** Weekday names are hard-coded (`nos/ui.c:914-917`). The format is always "SUN 11" (`faces.c:107-110`). There is no month, no locale and no date-format choice.
- **[low] Registry dispatch is hard-coded by pointer.** Style, input, interactive and press-rectangle dispatch compare face addresses (`faces.c:814-839`). `nos_face` has no id or flags (`face.h:29-36`). Adding a face means editing four places in `faces.c`. A reorder changes what a stored index means (see [[Face picker]]).
- **[low] `face_aod_shift` is an unused export with a misleading contract.** It is declared as the current burn-in drift (`face.h:40`), but nothing calls it (`faces.c:28-32`). The drift used on device is in `fk_aod_update`.

## Related systems

- [[Shell and faces]]: the host. Home page 0, the picker and the face contract.
- [[Engine and scheduling]]: frame clocks (40 Hz motion, 60 Hz present), the AOD and screen-off loops, and the tick dispatch.
- [[Services hub]]: the `svc_*` getters that fill `face_ctx`.
- [[Health and activity]]: steps, goal, heart and the SPLIT and SEGMENT rings.
- [[Sports and workouts]]: no workout data is read by the registry. Workouts show only through the REC chip, which is outside the face context.
- [[Power and battery]]: the battery pill, AOD thresholds and the 1 Hz screen-off cadence.
- [[Storage and persistence]]: the prefs blob, its SD banks, the NVS mirror and the ORRERY texture path.
- [[Hardware and drivers]]: PSRAM budget, the RTC-restored clock, the SD card and the optional MAX30102.
- [[Connectivity and voice]]: notification unread count and phone or Wi-Fi weather.
