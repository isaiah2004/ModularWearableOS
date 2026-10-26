---
title: "Developer tools"
id: app-developer-tools
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Developer tools

Launcher label **Developer tools** (`nos/apps/registry.c:34`, icon `DOT_ICON_GAUGE`). Root screen `scr_developer`, title **DEVELOPER TOOLS** (`nos/apps/set_developer.c:109-110`, `:143`). Sub-screens **FRAME RATE** (`scr_frame_cap`, `set_developer.c:107`) and **SYSTEM TRACE** (`scr_system_trace`, `set_developer.c:75`). Overlay engine `nos/developer.c` (41 lines), API `nos/developer.h`. Target: 1.43-B. The sources are compiled into the B firmware because the CMake glob takes all of `nos/*.c` (`firmware/main/CMakeLists.txt:14-15`).

## Summary

Developer tools is a thin UI over three things: four session-only on-screen overlays (loop rate, raster stats, touch crosshair, screen name) drawn over every screen; a persisted frame-rate cap for the UI pacer (24/30/40/48/60/Unlimited); and a read-only SYSTEM TRACE page that shows counters, reset reason, crash state and fault slot 0 of the checked diagnostic logger (`nos/diag_log.c`), plus one button that writes a test marker into that log. It calls no `svc_*` getter and no board or SD function directly.

On the 1.43-B the TE row is permanently N/A because the board routes no TE pin. Status is **partial**: overlays, frame cap and trace counters work, but the TE row is inert, SYSTEM TRACE shows only fault slot 0, the mark result is reported before the SD write, and the Unlimited mode has an idle-power cost that has not been measured on hardware.

## Features

1. **Overlay toggles.** Four rows, each flipping one bit of `nos_dev_overlays` and showing ON/OFF (`set_developer.c:108-115`, taps `:127-129`). Drawn by `nos_dev_draw()` (`developer.c:27-41`):
   - **Loop FPS** (`NOS_DEV_FPS=1`): "LOOP nn.n FPS", a 500 ms average of engine frame calls (`developer.c:15-18`, `:30`). It counts engine calls, not panel updates (`developer.c:1`).
   - **Frame stats** (`NOS_DEV_STATS=2`): "RASTER x.x MS  N PX" from `nos_last.raster_us` and `dirty_px`, refreshed every 500 ms (`developer.c:18`, `:31-34`).
   - **Touch position** (`NOS_DEV_TOUCH=4`): ring and cross at the last touch while pressed (`developer.c:20`, `:36-40`).
   - **Screen name** (`NOS_DEV_SCREEN=8`): top screen name and stack depth (`developer.c:35`).
2. **Turn all off.** Clears the four overlay bits only. Frame cap and TE are untouched (`set_developer.c:137`; asserted by `tests/developer_tools_test.c:60`).
3. **Frame rate.** Row shows the current cap (`set_developer.c:116-118`) and opens FRAME RATE (`:130`). Each option is a tap zone; a changed choice sets `nos_prefs.fps_cap` and calls `prefs_save()` (`:99-100`). "SET" marks the current value (`:89-90`). Option 0 is labelled "Unlimited" (`:80`).
4. **TE sync.** Row reads "N/A" in the disabled colour when `nos_dev_te_available()` is 0. When available it toggles `nos_prefs.display_te` and saves (`set_developer.c:119-121`, `:132-135`). When unavailable the tap is consumed with no feedback.
5. **SYSTEM TRACE** (`set_developer.c:75`, pushed from `:139`).
   - Summary page: Queued, Written, Dropped, Errors (`:50-53`); RESET name, mapped from the ESP reset enum (`:54-55`, names `:17-19`); PREVIOUS events and CRASHES (`:56-57`); RESULT request/value with EVIDENCE LOSS or NO KNOWN LOSS (`:59-61`).
   - Fault page (switch by tapping y 85-125, `:69`): First fault code, Occurrences, First ms, Last ms (`:41-44`); FAULTS n / MERGED n (`:45-46`); CRASH state and PRIORITY LOSS (`:47-48`).
   - Crash state is mapped to DISABLED, ARMED, SAVED, or CHECK REQUIRED (`:47`).
6. **ADD TEST MARK.** Button zone x 107-359, y 359-405 (`:62`, `:70`). Calls `diag_action_request(DIAG_DOMAIN_DEVELOPER, DIAG_ACT_MARK, diag_log_name_id("diag.marker"))`, then `diag_action_result(...)` (`:71-72`). Shows "MARK REQUEST n" and marks a tick (`:63`, `:73`).

## Reads

| Source | Where | Notes |
|---|---|---|
| `nos_dev_overlays` | `nos/developer.c:6` (definition); read `set_developer.c:114`, `developer.c:28-36` | RAM bitmask |
| `nos_prefs.fps_cap` | `nos/prefs.h:43`; read `set_developer.c:89`, `:116` | RAM copy of the shared prefs struct |
| `nos_prefs.display_te` | `nos/prefs.h:44`; read `set_developer.c:121` | RAM copy |
| `nos_dev_te_available()` | `nos/developer.c:8-10`: weak `hal_display_te_available()` | Firmware definition at `firmware/main/nos_main.c:967` = `nos_display_te_snapshot().ready` |
| TE snapshot | `firmware/main/nos_display_te.c:112-120` (C board) | On B `PIN_LCD_TE` is undefined, so the `#else` stub returns `ready=0` (`nos_display_te.c:3`, `:121-127`) |
| `diag_log_get()` | `nos/diag_log.c:326-327` | Full status copy; `queued` = queue plus priority depth |
| `diag_log_fault_get(0, ...)` | `nos/diag_log.c:247-256`; call `set_developer.c:22` | Index 0 only (see Gaps) |
| `nos_last.raster_us`, `dirty_px` | defined `nos/nos.c:29`; written `:2466-2476` | Copied every 500 ms (`developer.c:18`) |
| `nos_aod` | `nos/nos.c:47` | Overlay draw returns early in AOD (`developer.c:28`) |
| `nos_top()`, `nos_depth()` | `developer.c:35` | Screen-name overlay |
| `nos_revealed()`, `nos_leaving_covered()` | `nos/nos.c:121`, `:206`; used `set_developer.c:25`, `:28` | Trace enter and exit logic |
| `diag_log_name_id()` | `nos/diag_log.c:199-204` | Pure FNV-1a hash, no state |

No `svc_*` getter, no `hal_power`, no SD call and no `prefs_*` read beyond the two fields. Checked by grep of `set_developer.c` and `developer.c`.

## Writes

- **`nos_dev_overlays`** (RAM only): toggled `set_developer.c:128`; cleared `:137`; saved and restored by SYSTEM TRACE `:26`, `:28`. The simulator buttons also flip the bits (`sim/nos_sim.c:1841-1843`).
- **`nos_prefs.fps_cap`:** written `set_developer.c:100`, then `prefs_save()` (`nos/prefs.c:260`) marks the struct dirty. A debounced checkpoint follows (see Persistence). The main loop sees the change at `firmware/main/nos_main.c:2132-2140` and re-seeds the pacer.
- **`nos_prefs.display_te`:** written `set_developer.c:133`, only when TE is available. Applied by `display_preferences()` (`firmware/main/nos_main.c:968-974`), which calls `nos_display_te_set_enabled()`. That call is a no-op on B.
- **Diagnostics:** `diag_action_request()` sets `last_request` and `last_target` and posts `DIAG_ACTION_REQUEST` (`nos/diag_log.c:229-236`). `diag_action_result()` sets `last_result*` and posts `DIAG_ACTION_RESULT` (`:239-244`). Both go through `diag_log_event()` into the RAM queues and the RTC retain slots (`:218-228`, `:210-217`).
- **UI side effects:** `nos_invalidate()` (`set_developer.c:73`, `:102`, `:128`, `:133`, `:137`), `nos_feedback(FB_TICK)`, and `nos_push(&scr_frame_cap | &scr_system_trace)` (`:130`, `:139`).
- **Module statics only:** `trace_status`, `trace_fault`, `trace_page`, `trace_marked`, `trace_mark_request` (`set_developer.c:9-14`).

## Background behaviour

- **Overlay measurement** runs in `frame_run()` for every non-readonly engine frame (`nos/nos.c:2302`), whether or not an overlay is on. Those frames happen only in the ON branch (`firmware/main/nos_main.c:2155`). AOD uses `nos_frame_run_readonly()` (`nos_main.c:2116-2117`), which skips it, so LOOP FPS freezes off-screen.
- **Overlay drawing** is global. `draw_overlays()` (`nos/nos.c:1764`) is called on every composed frame (`nos.c:1788`, `:1801`, `:1876`). Closing DEVELOPER TOOLS does not hide overlays; they stay on every screen until "Turn all off" or a reboot.
- **Touch feed:** `nos_dev_touch()` is called from `nos_touch_at()` (`nos/nos.c:659`) before the lock check (`:666`), so the crosshair follows touches even while locked.
- **SYSTEM TRACE tick:** `trace_tick()` runs once per frame while the page is on top (`nos/nos.c:2104-2108`, tick_screens at `:2347`). Each call does `diag_log_get()` and `diag_log_fault_get(0)` (`set_developer.c:22`). The root DEVELOPER TOOLS screen has no tick (`set_developer.c:143`).
- **Diagnostics producers** run regardless of screen state. `diag_log_tick()` is called at the top of every `app_main` loop pass (`firmware/main/nos_main.c:1982`), in the OFF branch (`:2091-2103`) and in the AOD branch (`:2104-2130`). It samples power, heap and DMA heap every 30 s (`nos/diag_log.c:354-368`) and posts fault summaries and drop events (`:371-387`).
- **Diagnostics SD writer** is the `sports_sd` task (`nos/sports.c:282`, created `:323`: priority 1, 4 KB stack in PSRAM, no core affinity). Each pass it calls `diag_log_service()` (`sports.c:289`), then sleeps 1000 ms, or 20 ms while a sports query is pending (`sports.c:291`). `diag_log_service()` returns at once when nothing is queued (`nos/diag_log.c:465-471`). With the screen off, the card is opened, appended, fsynced and closed whenever records are queued (`:483-520`).
- **Prefs checkpoint** for fps_cap and display_te: `prefs_tick()` runs from ON frames (`nos/nos.c:2349`), from the ON loop (`firmware/main/nos_main.c:2164`) and from the OFF branch (`:2098`). A dirty struct flushes 2 s after the last edit or 10 s after the first unsaved edit (`nos/prefs.c:379-381`; `nos/prefs.h:55`).
- **Jobs record for System trace** samples `diag_log_get()` through `sample_services()` (`nos/background.c:142-145`). `background_tick()` throttles to 1 s (`background.c:160-164`). It is driven by ON frames (`nos/nos.c:2341`) and once per second in OFF (`firmware/main/nos_main.c:2094`).
- **Arithmetic from the code, not measured:** the timed samples alone are 3 routine records of 44 B every 30 s, about 380 KB/day (`nos/diag_log.c:354-368`, `:42-49`). Eight 1 MiB files would last about 22 days before rotation reuses a slot (`:396-462`).

## Persistence

| Item | Where | Survives reboot |
|---|---|---|
| Overlay bits | RAM `nos_dev_overlays` (`nos/developer.c:6`). Not a `nos_prefs` field (`nos/prefs.h:7-46`). | No |
| Frame cap | `nos_prefs.fps_cap` (`nos/prefs.h:43`, V4 byte, `nos/prefs.c:14`). Default 60 (`prefs.c:161`). Invalid values normalized (`prefs.c:188`). | Yes |
| TE flag | `nos_prefs.display_te` (`nos/prefs.h:44`, V5 byte, `prefs.c:16`). Normalized to 0/1 (`prefs.c:189-190`). | Yes (inert on B) |

Both fields live in the shared `nos_prefs_t`. The checkpoint is `hal_store_save_checked("nosprefs")` (`nos/prefs.c:349`). On B it writes the checksummed alternating banks `/sdcard/watch/settings.0.bin` and `settings.1.bin` (`nos/prefs_disk.c:32`; dir `firmware/main/hal_esp32.c:243`). It also mirrors to NVS namespace `wos`, key `nosprefs` (`firmware/main/hal_esp32.c:228-256`). Load falls back to NVS only when the SD status is MISSING (`hal_esp32.c:258-283`, NVS read `:277`). Edits made while the card is unresolved are captured per field (`nos/prefs.c:69`) and merged onto the loaded struct (`nos/prefs.c:215-220`).

Diagnostics (consumed by SYSTEM TRACE and the test marker):

- **SD:** `/sdcard/watch/diagnostics/diag-00.bin` through `diag-07.bin` (`nos/diag_log.c:24`, `:394-395`). Each file has a 32-byte DLGF header and 44-byte DLG1 records with CRC32 (`:59-71`). Files are capped at 1 MiB (`:49`) and rotate by generation (`:396-462`). The directory is created with `mkdir` (`:484-486`). Writes need the board SD lease and the sports SD lock (`:473-480`).
- **RAM:** queues of 128 routine and 32 priority records, in PSRAM (`diag_log.c:42-43`, `:86-87`, `:263`). Overflow drops with an evidence-loss flag (`:153-164`).
- **RTC (`RTC_NOINIT`):** last 32 records (`:74`), 8 fault slots (`:83`), loss counters (`:84`), crash-boot count (`:72-73`). These survive supported warm resets, not power loss (`nos/diag_log.h:44-46`, `:58-60`).
- **Test marks** persist only once the record reaches SD.

## Navigation

- **Opened from** the launcher app list (`nos/apps/registry.c:34`). `nos_open()` (`registry.c:45-53`) records the last app and pushes with `TX_IRIS`.
- **Opened from** Jobs: the "System trace" detail routes to `scr_developer` (`nos/apps/sys_background.c:217`, pushed `:245`). It lands on the DEVELOPER TOOLS root, not on the SYSTEM TRACE page.
- **Not linked** from Settings or Diagnostics (no reference in `nos/apps/set_settings.c` or `set_diag.c`).
- **Pushes** FRAME RATE (`set_developer.c:130`) and SYSTEM TRACE (`:139`).
- **Inside SYSTEM TRACE:** tap the status band (y 85-125) to switch Summary and Faults (`:69`). The hint "TAP STATUS: SUMMARY / FAULTS" (`:64`) disappears after the first mark (`:63`). Nothing on the page says the band is tappable.
- **No in-screen back control** exists in the source (`set_developer.c:109-142`). Exit is the engine navigation gesture.
- **Simulator** buttons toggle the same overlay bits (`sim/nos_sim.c:1841-1843`).
- **Serial bench commands** (not user-facing): `te 0|1` (`firmware/main/nos_main.c:1216-1222`; `te 1` answers "ERR te unavailable" on B at `:1218`), `fps_cap [N]` (`:1265-1275`), `diag log` (`:1204`), `NOSTE` (`:1001-1007`).

## Gaps

1. **medium. TE sync row is inert on the 1.43-B.** Evidence: the row reads N/A (`nos/apps/set_developer.c:119-121`). Taps in y 285-336 are consumed with no effect or feedback (`:132-135`). B has no TE pin (`firmware/main/pins.h:45`; `PIN_LCD_TE` only under the C branch, `pins.h:19`), and the B stub returns `ready=0` (`firmware/main/nos_display_te.c:121-127`; `firmware/main/nos_main.c:967`). User impact: one of five rows does nothing on the active board, and the N/A label gives no reason.
2. **medium. Unlimited has no idle yield and an unmeasured battery cost.** Evidence: with cap 0, `nos_frame_pacer_capped()` returns `completed` (`firmware/main/nos_frame_pacer.h:64`), so there is no timer deadline. The loop sleeps only when a frame painted nothing: `vTaskDelay(1)` (`firmware/main/nos_main.c:2289`). With `CONFIG_FREERTOS_HZ=1000` (`firmware/sdkconfig:1917`) that is 1 ms. A static ON screen would therefore run `nos_frame_run()` about every 1 ms, against a 16.7 ms period at 60 fps (`nos_main.c:2155`, `:2132-2135`). The picker label says only "Unlimited" (`set_developer.c:80`). LOOP FPS would also count those idle calls (`nos/developer.c:15-18`). This is an inference from code. It needs a hardware measurement before anyone relies on it.
3. **medium. SYSTEM TRACE shows fault slot 0 only.** Evidence: `set_developer.c:22` calls `diag_log_fault_get(0, ...)`. The API supports 8 slots (`nos/diag_log.c:44`, `:247-249`), and the full list appears only in the host `diag log` report (`nos/diag_log.c:340-346`, `firmware/main/nos_main.c:1204`). User impact: FAULTS n can count faults the page never shows, and only the first fault's code and count are visible.
4. **medium. No on-watch view, export, clear or retention status for the diagnostics journal.** Evidence: the schema is documented for the host exporter `tools/diag_export.py` (`nos/diag_log.h:44-46`). The screen has no export, clear or share action (`nos/apps/set_developer.c:62`, `:137`). User impact: up to 8 MiB of SD journal (`nos/diag_log.c:47-49`) can only be read from a PC with the card.
5. **low. ADD TEST MARK reports SUCCESS before the write, and never reports FAILED.** Evidence: `set_developer.c:71-72` passes SUCCESS whenever the request ID is nonzero. That ID only means the record was accepted into RAM. `diag_log_event()` ignores the result of `enqueue()` (`nos/diag_log.c:226`), and a priority overflow still reads SUCCESS (`:153-164`). When the logger is not ready, `diag_action_request()` returns 0 (`:231`). `diag_action_result(0, FAILED, 0)` then returns at once (`:241`), so FAILED is never stored. The RESULT line keeps the previous result, while the mark row shows "MARK REQUEST 0" (`set_developer.c:63`). This conflicts with the header contract that "result records actual completion" (`nos/diag_log.h:52-54`).
6. **low. Fault and crash details are raw numbers.** Evidence: the first-fault code prints as decimal (`set_developer.c:41`), so DIAG_DISPLAY_FAULT (0x10e) shows as 270. No name table is on the device; names exist only in host tooling. Crash PC and build are not shown; the page shows only the crash state (`set_developer.c:47`). The host report prints both (`nos/diag_log.c:335-339`).
7. **low. Touch crosshair can stick after a cancelled contact (plausible, not reproduced).** Evidence: `nos_touch_at()` returns on `down==2` before calling `nos_dev_touch()` (`nos/nos.c:658-659`). The lost-contact path does push `down==2` (`firmware/main/nos_main.c:571-572`). `down` is cleared only by a later normal touch (`nos/developer.c:20`). User impact: a stale ring on screen until the next touch.
8. **low. "Turn all off" clears overlays only.** Evidence: `set_developer.c:137` clears `nos_dev_overlays` and nothing else. Frame cap and TE stay as they were (`tests/developer_tools_test.c:60`; `docs/NOS_SIM.md:461` matches the code). The label reads like a reset of the whole screen.
9. **low. The frame-rate picker gives no achieved-rate readback.** The only rate figure is LOOP FPS, which counts engine calls rather than panel updates (`nos/developer.c:1`). It is off until enabled separately (`set_developer.c:108`). The picker shows only "SET" (`:89-90`). Caps "do not guarantee tear-free output" (`docs/NOS_SIM.md:461`).
10. **low. Overlays are global and have no indicator.** `draw_overlays()` runs on every screen (`nos/nos.c:1764`, `:1788`). Nothing on the active screen shows that a bit is set, and the only off switch is inside this screen.
11. **low. Jobs "System trace" opens the DEVELOPER TOOLS root.** Evidence: `nos/apps/sys_background.c:217`, `:245`. Already noted in the Jobs app note (`docs/os-map/apps/Jobs.md:129`).
12. **low. "Write errors" counts attempts, not distinct failures.** With the card absent and records queued, each `diag_log_service()` call increments `errors` and posts DIAG_STORAGE_ERROR (`nos/diag_log.c:473-479`). It is called every 1 s when idle, or every 20 ms while a sports query is pending (`nos/sports.c:289-291`). The fault slot coalesces (`diag_log.c:166-190`), so FAULTS stays at one while MERGED and Errors climb (`set_developer.c:45`, `:53`).
13. **low. Records carry epoch 0 until system time is valid.** `hal_epoch_seconds()` returns 0 before 2024-01-01 (`firmware/main/hal_esp32.c:148-153`), and `diag_log_event()` stores it as-is (`nos/diag_log.c:221-222`). Journal ordering then depends on uptime alone.
14. **low. Feature scope against a modern watch's developer options.** Missing: animation scale, stay-awake or charging toggle, layout bounds, an on-watch heap or CPU graph (heap samples go to SD only, `nos/diag_log.c:362-367`), a share or bug-report action, and any gate on the launcher entry (`nos/apps/registry.c:34` is always visible).
15. **low. Docs drift.** `docs/os-map/systems/Settings and developer.md:173` says developer screens tick "up to 60 Hz while the screen is on". With Unlimited the loop is not paced at 60 Hz (gap 2). The rest of that note's Developer tools section (lines 72-83) matches the code.

Related: [[Engine and scheduling]] (frame loop, pacer, tick order), [[Storage and persistence]] (prefs checkpoint and diagnostics SD files), [[Power and battery]] (Unlimited idle cost, OFF/AOD loops), [[Hardware and drivers]] (B has no TE pin, SD pins 41/39/40, battery ADC), [[Sports and workouts]] (the `sports_sd` task that runs the diagnostics writer), [[Shell and faces]] (launcher entry and navigation), [[Services hub]] (shared 1 Hz tick; no direct getters).
