---
title: "Orrery / Solar face"
id: face-orrery
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Orrery / Solar face

Source: `nos/faces/face_orrery.c` (2661 lines), `nos/faces/orrery_card.c` (2298), `nos/faces/orrery_events.c` (147), `nos/faces/orrery_data.c` (4580, generated tables). Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from source reading only, not from the device.

Coverage: `face_orrery.c` and `orrery_events.c` were read in full. `orrery_data.c` was read as generated tables (header, counts, small-body, comet, craft and flyby sections). `orrery_card.c` was read at its entry, tick, input, asset-loading, body-build and page-dispatch sections; its raster code was grepped, not read line by line. The included helpers (`face_orrery_*.h`, `orrery_card_*.h`, `orrery_card_*.inc`, `orrery_shade_q16.c`, `orrery_basis*.h`) were grepped for HAL, service, prefs and storage calls. None of the orrery sources call `svc_*` or `prefs_*`.

## Summary

ORRERY is the ninth face, index 8, in the home face list (`nos/faces/faces.c:13-17`). It draws the solar system at the current time: the clock sits over the Sun, eight planets sit on rank-spaced orbits at their heliocentric longitudes, and the asteroid belt, Kuiper belt, named small bodies, comets and spacecraft are drawn around them. Unlocked, it takes all touch: pan, look, time travel, zoom, tilt, a menu and three date dials. It has two sub-screens: the planet card (`scr_orrery_card`, `ORRERY CARD`) and the events list (`scr_orrery_events`, `ORRERY EVENTS`).

The face owns no task and no timer. Home ticks it on the UI loop at 40 fps while home is the idle top screen (`nos/nos.c:2100-2110`, `nos/render_config.h:8`, `nos/apps/sys_home.c:625`). Its state is one ~88 KiB PSRAM block (`nos/faces/face_orrery.c:346-361`). Nothing is persisted.

Hardware needed: PSRAM for all large buffers (`hal_big_alloc` is `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, `firmware/main/hal_esp32.c:1595-1597`); the 466 x 466 panel and touch, both through home; the ESP system clock (`hal_epoch_seconds`, `firmware/main/hal_esp32.c:148-151`); an SD card for the card's photo textures (`hal_sd_root()` = `/sdcard`, `firmware/main/hal_esp32.c:1592`). Feedback goes through `nos_feedback` (FB_TICK, FB_TAP, FB_TOGGLE_ON/OFF). The face reads no IMU, Wi-Fi, BLE, GPS or battery data. The B board has 8 MB PSRAM and a one-bit SD (CLAUDE.md, hardware table).

## Features

- **Live sky.** Wall time comes from `unix_now` (`face_orrery.c:1141-1147`, `1180-1182`). Planet positions come from `ep_helio` in `core/ephem.c` (`face_orrery.c:688-698`).
- **Orbits and bodies.** Planet orbits are 128-point ellipses (`NORB`, `face_orrery.c:49`). The belt has 3999 dots (`od_belt_n`). The belt refresh budget is 120, 64 or 448 bodies per tick depending on motion (`face_orrery.c:633`). Also drawn: 23 named small bodies, 21 comets with tails, and 16 spacecraft (`face_orrery.c:912-928`).
- **Zones.** Overview (`V_FACE`), inner system, outer system and beyond Neptune (`face_orrery.c:90`, radial maps `:118-134`). The beyond-Neptune map is logarithmic.
- **Time travel (unlocked).** Drag is PAN, LOOK or TIME (`face_orrery.c:2476-2481`). TIME grabs a planet and moves at its pace. Events act as magnets within about 2.2 degrees of the grabbed planet's travel (`snap_check`, `face_orrery.c:816-826`). Range is 1800 to 2050 (`core/ephem.h:16-17`; toast `TIME RANGE 1800-2050`, `face_orrery.c:368-374`).
- **Time dials.** An inward swipe from the top-right rim opens three date rings (years, months, days). Drag a ring to travel, tap a value to fly there, NOW returns home (`face_orrery.c:1015-1107`, `2322-2328`, `2366-2380`).
- **Zoom dial.** Bottom rim, 4:30 to 7:30, 0.5x to 4x, with a tick at each mark (`face_orrery.c:57-65`, `2493-2499`).
- **Menu.** Swipe left from the right rim. DRAG (PAN, LOOK, TIME, RECENTRE), VIEW (OVERHEAD, TILT), FILTER (PLANETS, ASTEROIDS, COMETS, CRAFT, FROM EARTH = five nearest bodies), LINES (`face_orrery.c:1110-1138`, `2265-2289`). Every option has a handler.
- **3D tilt and lock.** Hold 0.75 s toggles 3D (`HOLD_TILT`), 1.5 s toggles lock (`face_orrery.c:2434-2438`). The side button locks (`:2539-2542`). Sleep locks and eases back to home (`:1187-1193`). Locked, touches go to home, and a tap shows `HOLD 1.5 S TO UNLOCK` (`:2447-2452`, `:2548-2553`).
- **Next-event chip.** Shows the active or next scanned event, as `IN n D`, `TODAY` or `n D AGO` (`face_orrery.c:1260-1275`). Tapping it opens the events list, or flies to NOW if time has been travelled (`:2305-2311`). The event geometry is drawn in red (`draw_event`, `:1554-1575`).
- **Event scan.** Oppositions, conjunctions, greatest elongations, inferior and superior conjunctions and line-ups. Minimum rank 2. Window is 60 days back to about 400 days forward, up to 460 days (`face_orrery.c:764-803`, rank check `core/ephem.c:209-212`).
- **Planet card.** Tap a planet, Pluto or the Sun (`face_orrery.c:2329-2332`). Pages are globe (PHOTO, HEAT or DOT modes, FREE rotation), HEAT profile, MOONS and CRAFT (four-page bodies only), and FACTS. Pages swipe; the dial spins (`orrery_card.c:2159-2267`, page map `:1743-1752`).
- **Events list.** Up to 48 rows, each with date, days to go and title. The first row has the red dot. Tap a row to fly the face's time there and pop back (`orrery_events.c:44-118`).
- **AOD.** Eight planet dots and HH:MM, locked at home (`face_orrery.c:1968-1975`).

## Reads

Per-frame context from home, built in `build_ctx` (`nos/apps/sys_home.c:465-490`):
- `hal_get_datetime()` into `c->dt` (`sys_home.c:468`, `firmware/main/hal_esp32.c:142-146`). Used for clock text, the AOD minute and the wall-clock fallback.
- `svc_notif_unread()` into `c->unread` (`sys_home.c:476`, `nos/svc.c:238`). Drives the top unread dot through `fk_unread` (`face_orrery.c:2219-2225`, `nos/faces/faces.c:552-566`).
- `c->aod`, `c->rec`, `c->enter`: engine flags.

Direct HAL and prefs:
- `hal_epoch_seconds()` (`face_orrery.c:1143`, `:1183`; `orrery_card.c:264`, `:1931`). This is the ESP system clock. It returns 0 unless the time is between 2024 and 2100 (`firmware/main/hal_esp32.c:148-151`). Zero means the face falls back to `c->dt` (`face_orrery.c:1145-1146`).
- `hal_big_alloc` and `hal_big_free` (`face_orrery.c:360`, `:450`, `:525-528`, `:2110`; `face_orrery_ringcache.h`, `face_orrery_platecache.h`, `face_orrery_runs.h`; `orrery_card.c`). All PSRAM.
- `hal_millis()` (card asset retry timers, `orrery_card.c:730-771`) and `hal_micros()` (profiling, `face_orrery_profile.h`).
- `hal_sd_root()` = `/sdcard` (`firmware/main/hal_esp32.c:1592`; used at `orrery_card.c:776`).
- Asset worker: `hal_asset_submit`, `hal_asset_take`, `hal_asset_poll`, `hal_asset_busy`, `hal_asset_cancel` (`orrery_card.c:673-799`; `firmware/main/hal_esp32.c:2500-2520`).
- `nos_prefs.hour24` (`face_orrery.c:2021`, read only as a cache key). The 12 or 24 hour display is applied in `nos/faces/faces.c:81-89`.
- Engine UI: `nos_feedback`, `nos_toast`, `nos_open`, `nos_pop`, `nos_top`, `nos_transitioning`, `nos_revealed`, `nos_touch_travel`, `nos_t`.

Compiled-in sky pack (`nos/faces/orrery_data.c`, header `nos/faces/orrery_data.h`):
- `od_belt[3999]` (`orrery_data.c:4010`), `od_small_bodies[23]` (`:4037`), `od_comets[21]` (`:4062`), `od_crafts[16]` (`:4082`), `od_moons[460]` (`:4546`), `od_flybys[30]` (`:4580`).
- `od_pack_jd = 2461327.5` and `od_built = "2026-10-14"` (`orrery_data.c:5-6`).

Ephemeris maths (`core/ephem.c`, `core/ephem.h`): `ep_helio`, `ep_helio_anomaly`, `ep_conic_*`, `ep_sat_pos`, `ep_moon`, `ep_observe`, `ep_scan_*`, `ep_event_title`. The file has no HAL, storage or prefs calls (grep count 0). `ep_observe` takes heliocentric and Earth vectors only (`core/ephem.h:69`), so no observer location is used.

Card static data: `nos/faces/orrery_card_data.inc`. Heat profiles are at lines 4-16 and facts at 17-29, labelled prototype samples at line 1. Texture names are `orrery_card.c:238-239`.

Events list: reads `orrery_now()`, `orrery_events()`, `orrery_events_generation()` and `orrery_events_progress()` from the face (`face_orrery.c:2633-2661`). Also reads `od_flybys` and `od_comets` directly (`orrery_events.c:57-71`).

Services: none directly. A grep for `svc_` and `prefs_` across the orrery sources returns nothing.

## Writes

No writes to services, prefs, NVS or SD. A grep finds no `prefs_*` call, no `svc_*` mutator, no write-mode `fopen`, and no `hal_*` setter in any orrery source.

Face-owned RAM state (`orr_state S`, `face_orrery.c:253-345`):
- Time: `S.off`, `S.jd`, `S.vel`, `S.ta_*`.
- View: `S.view`, `S.zoom`, `S.panx`, `S.pany`, `S.tilt`, `S.cam_tilt`, `S.az`.
- Lock and filters: `S.locked`, `S.show[4]`, `S.from_earth`, `S.lines`, `S.drag_mode`.
- Events: `S.ev[64]`, `S.nev`, the scan state.
- Belt and small-body arrays in PSRAM, time-dial and menu state.
- First init sets locked, zoom 1, lines on and all four show flags on (`face_orrery.c:360-364`).

Card and events RAM: card state `C.*` (`orrery_card.c`), textures in PSRAM (`TEX_BYTES` = 1 MiB, `orrery_card.c:16`), and the events array `E` (`orrery_events.c:17-19`).

Feedback and toasts only: `nos_feedback`, `nos_toast`.

Serial bench output only: `orrery_report_state` (`face_orrery.c:2595-2601`), `orrery_basis_dump` (`:2605-2632`), and the `orrery state`, `orrery basis mode`, `orrery basis check` and `orrery basis dates` commands (`firmware/main/nos_main.c:1564-1580`). These print to serial and set basis globals in RAM (`face_orrery.c:495-499`).

Boot: `dotbake_task` runs `orrery_basis_check()` and `orrery_shade_q16_prebake()` once, off the UI thread (`firmware/main/nos_main.c:497-506`). It writes only RAM validation flags.

## Background behaviour

Per power state, who runs what:

- **Awake, home idle.** The face ticks every frame at 40 fps from the UI loop (`nos/nos.c:2100-2110`, `nos/apps/sys_home.c:625`). Each tick runs `sky_update` (planet ephemerides at a 0.05 day cadence while still, exact during motion, `face_orrery.c:663-703`), a belt slice (`belt_update`, `:617-660`), 2 or 8 small-body solves (`:733-740`), and a 4-day scan step (`events_scan_step`, `:774-788`). The first belt basis is built on the first tick (`belt_init`, `:520-560`).
- **Card or events list on top, idle.** Home is not ticked (`nos/nos.c:2100-2110`), so the face's sky, belt and scan freeze. The card advances its own date from `hal_epoch_seconds()` since it opened (`orrery_card.c:264-265`). The events list runs scan slices from its own tick (`orrery_events.c:126-132`, `orrery_events_step` at `face_orrery.c:2650-2655`). Its rows rebuild only when the generation changes (`orrery_events.c:129-130`).
- **Screen off (PWR_OFF).** The face does not tick. The power loop calls `nos_bg_tick(1.0f)` once a second (`firmware/main/nos_main.c:2094`). That is the services hub, not the face. On wake, the face recomputes from wall time (`face_orrery.c:1180-1182`).
- **AOD (PWR_AOD or PWR_SENSOR).** `nos_frame_run` runs once a second (`firmware/main/nos_main.c:2116`). Home ticks the face with `c->aod = 1`, so `sky_update` updates planets only (`face_orrery.c:706`). `events_update` is skipped (`:1215`). The face forces lock, home, zero offset and no menus (`:1187-1193`). It draws eight dots and HH:MM (`:1968-1975`).
- **Asset worker `asset-read`.** FreeRTOS task at priority 1, 6144-byte stack from the engine allocator, no core affinity, created on the first texture request (`firmware/main/hal_esp32.c:2465-2497`). It reads 1 MiB textures in pages from SD under the board SD lock (`:2437-2460`). It runs only while a card body's textures load (`orrery_card.c:728-800`). Leaving the card cancels pending loads unless the screen is covered (`orrery_card.c:1945-1948`).
- **Memory trim.** `orrery_cache_idle` runs after 5 s without a draw (`face_orrery.c:2574-2577`, `nos/psram.c:159-167`). `orrery_cache_card_prepare` frees the ink accelerator (`face_orrery.c:2565-2573`, `nos/psram.c:141-148`). Opening Settings trims scratch (`nos/apps/set_settings.c:1234-1236`).

Threading: all face and card work runs on the UI thread. The only other work is the `asset-read` task and the boot prebake. The face comments call the scan slices "UI-owner" (`orrery_events.c` header, `face_orrery.c` comments).

## Persistence

Nothing is persisted by the face or the card.

- **NVS:** no keys written. The grep finds no `nvs_*` or `prefs_*` call in the orrery sources.
- **SD, read only:** `/sdcard/sky/<name>.565` for sun, mercury, venus, earth, earth_night, moon, mars, jupiter, saturn, uranus and neptune (1 MiB each), plus `saturn_ring.rgba` (2 KiB) (`orrery_card.c:744-777`). Pluto has no texture (`orrery_card.c:239`). The host copy is in `build/sd/sky`, pushed to the card by `tools/sdsync.py` (its header, line 14). It is not in flash.
- **Prefs:** the face is chosen by the face picker, which writes `nos_prefs.face` (see [[Face picker]]). The orrery reads only `nos_prefs.hour24`.
- **Session state:** lock, zoom, pan, tilt, view, filters, drag mode, time offset and the event list are RAM only. The face boots locked, at home, zoom 1, lines on, all filters on (`face_orrery.c:360-364`). Each AOD entry resets the time offset and view (`:1187-1193`).
- **Events scan:** recomputed each boot. It starts on the first face tick (`face_orrery.c:2652-2654`, when `S.scan_start` is 0).

## Navigation

**Opened from:**
- Home pager, face index 8 (`nos/faces/faces.c:13-17`). Reached through the face picker, which also opens from the Settings "Watch face" row (see [[Face picker]]).
- Face picker, by a 3 s hold on the face: `home_open_faces()` (`face_orrery.c:1253-1255`, `nos/apps/sys_home.c:1008-1013`).

**Opens:**
- ORRERY CARD: tap a planet (`face_orrery.c:2329-2330`), Pluto (`:2331`) or the Sun (`:2332`). Call path `orrery_open_card` (`orrery_card.c:2276-2280`), flags `SCR_HDRAG | SCR_NATIVE_CACHE` (`:2275`).
- ORRERY EVENTS: tap the chip when at NOW or unlocked (`face_orrery.c:2305-2311`), through `orrery_open_events` (`orrery_events.c:147`).
- Tapping a small body, comet or craft shows a toast readout instead (`face_orrery.c:2333-2351`).

**Returns:**
- Events list: tapping a row calls `orrery_jump(jd)` then `nos_pop()` (`orrery_events.c:112-118`).
- Card: no close control of its own. Its input (`orrery_card.c:2159-2267`) handles pages, dial spin and taps. Leaving relies on the engine's screen-pop gesture, which was not traced.
- Face: an edge swipe from the left half returns to the overview (`face_orrery.c:2476`, `:2536`). A tap on the top area resets to the overview (`:2358`).

**Touch routing:** while the face fills the glass, home offers each touch to `face_orrery_input` first (`nos/apps/sys_home.c:1020-1028` to `nos/faces/faces.c:826-828`). Home keeps the hardware button when locked (`face_orrery.c:2539-2540`).

## Gaps

1. **Events list can silently drop rows.** (medium) `orrery_events.c:17` caps the list at 48 rows and `add()` drops anything past it (`orrery_events.c:34-38`). Scanned events are added first (`:56`), then BepiColombo, flybys and comets (`:57-71`), then sorted (`:72`). The scan itself caps at 64 (`face_orrery.c:56`, `core/ephem.c:211`). If the scan yields about 48 or more future rows, late-added flybys and comets are discarded before the sort. Not measured on the device. User impact: flybys and comets go missing from the list.
2. **No event alerts.** (medium) The scan runs only from the face tick (`face_orrery.c:1215`) and the events-list tick (`orrery_events.c:128`). Nothing runs in AOD or screen-off, and no service or alarm hook exists in the orrery sources. Users are not told about an opposition or flyby unless they open the face.
3. **Sky pack frozen at 2026-10-14.** (medium) `orrery_data.c:5-6`. Small bodies, comets, craft and moons use fixed elements. The file is generated ("Do not edit", `orrery_data.c:1`) and is rebuilt on the host (`nos/faces/orrery_data.h:2`). No refresh path exists on the watch. The flyby table ends at JD 2461524.6 (`orrery_data.c:4580` region), around late April 2027, so flyby events stop after that.
4. **Hard-coded status text goes stale.** (low-medium) `orrery_card.c:1698-1699` reads "NO ORBITER SINCE AKATSUKI / ENDED / SEP 2025". Juno's status "status unclear since Sep 2025" is at `orrery_data.c:4070`. BepiColombo's insertion date 21 Nov 2026 is hard-coded as JD 2461365.5 in the events list (`orrery_events.c:57-59`) and the card countdown (`orrery_card.c:530-531`). Its craft status is at `orrery_data.c:4071`. These go wrong once the events pass or the status changes.
5. **Card photos depend on SD textures that do not ship with the firmware.** (medium) `orrery_card.c:776` reads `/sdcard/sky/<name>.565`. The textures exist only in host `build/sd/sky` and reach the card by manual `tools/sdsync.py` push (`tools/sdsync.py:14`). Without them the card shows `[ PHOTO UNAVAILABLE ]` (`orrery_card.c:1658`) and a tinted sphere. This is a working fallback, not a crash.
6. **Card heat and facts are prototype samples.** (medium) `orrery_card_data.inc:1` labels them "Prototype profile samples and facts". The heat curves are hand-entered (lines 4-16) and the facts are typed strings (lines 17-29). The card shows some of them as numbers, for example "CLIMATE MEAN / MODEL" (`orrery_card.c:1642`). They are not computed from the sky pack or measured data.
7. **No "clock not set" state.** (low) If the system clock is unset, `hal_epoch_seconds()` returns 0 (`firmware/main/hal_esp32.c:148-151`). The face then uses `c->dt` (`face_orrery.c:1145-1146`), and the zone offset stops updating (`:1183-1186`). The face shows no warning. The clock comes from the RTC restore at boot (`firmware/main/board.c:875`) or the manual set in Settings (`nos/apps/set_settings.c:2361`).
8. **Face state is not persisted.** (low) Lock, zoom, filters, view and drag mode reset on every boot (`face_orrery.c:360-364`). Each AOD entry also resets time and view (`:1187-1193`). A user's chosen filters and 3D tilt are lost on reboot.
9. **AOD is minimal.** (low) AOD draws eight dots and HH:MM (`face_orrery.c:1968-1975`). It shows no date, no next event and no battery.
10. **No observer or location features.** (low) All geometry is heliocentric or Earth-centred. No orrery source reads a latitude or longitude (`core/ephem.h:69`). There are no rise or set times and no local sky. Lunar phase appears only on the card (`orrery_card.c:421-423`).
11. **Card next-event line may be empty.** (low) The card's NEXT line reads `orrery_events()`, which returns only the events scanned so far (`orrery_card.c:627-640`). It can show nothing, or the wrong event, until the scan completes.

No dead buttons found. Every menu option has a handler (`face_orrery.c:2278-2287`), and the card's page and dial controls are wired.

## Related

[[Shell and faces]] (home pager, face host), [[Engine and scheduling]] (tick dispatch, sleep and AOD, screen stack), [[Storage and persistence]] (the face persists nothing; SD textures), [[Hardware and drivers]] (PSRAM, SD, system clock, feedback), [[Services hub]] (the unread dot and the screen-off services tick), [[Power and battery]] (AOD and screen-off paths), [[Sports and workouts]] (REC chip drawn by home over this face), [[Connectivity and voice]] (no phone or time-sync path in the orrery sources), [[Face picker]], [[Home pager]].
