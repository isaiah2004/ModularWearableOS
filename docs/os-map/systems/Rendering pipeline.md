---
title: "Rendering pipeline"
tags: [nos, os-map]
updated: 2026-10-23
---

# Rendering pipeline

## Summary

NOS on the 1.43-B draws with a retained-mode, damage-driven, stripe rasterizer. Each frame the whole scene draw list is run once in "record" mode (`gfx_rec`, 320 commands of bbox + hash) and diffed against the previous recording (`compute_damage`, at most 12 rects, snapped to 16-row stripes; nos/nos.c:1942). Only damaged 16-row stripes are rasterized, by re-running the scene draw list per stripe into one of two internal-DRAM stripe buffers. A flush task on core 1 rotates each stripe 90 degrees CCW with a byte swap into two 8-row DMA buffers and pushes it over QSPI (80 MHz, CO5300 init profile) to the panel (firmware/main/nos_main.c:289-373). There is no TE line on B, so `nos_display_te_wait` is a no-op (firmware/main/nos_display_te.c).

Key facts about what the code really does (not what comments/docs claim):

- The shipping device build is sharp-only: `NOS_RENDER_SHARP=1` is set in firmware/main/CMakeLists.txt:73. That disables motion blur (mblur), iris blur, temporal exposure/history, the ui_blur directional cache and pblur on the device. The sim default is SHARP=0, so the sim shows blur the watch never renders. `NOS_MBLUR_BUDGET_US 26000` (nos/nos.h:123) is device-inert.
- Pacing defaults: `NOS_PRESENT_FPS` 60, authored clock `NOS_FPS` 40, floor `NOS_MIN_FPS` 30 (nos/render_config.h). fps_cap options {0,24,30,40,48,60}. The "30 fps with motion blur" budget in docs applies to the sim; on device the budget is spent on stripe re-raster and the flush chain.
- The UI task is the app_main "main" task on core 0 at priority 1 (lowest non-idle), so it sits beneath esp_timer (22), NimBLE host (21), Wi-Fi and lwip tcpip (18). Flush (prio 4) and touch (prio 5) are pinned to core 1.
- Buffers: stripe/DMA buffers in internal DRAM; layer rasters, dot masks, kblur arena, native-menu unpack arena, asset scratch in PSRAM.
- Legacy and not built into the NOS image: firmware/main/main.c, core/wos.c, core/apps/*, core/pip*.c. firmware/main/display_perf.c and display_bench.c are compiled but referenced only by legacy main.c (dead in the NOS build).

## Components

### Frame loop and screen state machine (eng-frame-loop)
- Purpose: `app_main` main loop; per-iteration state (ON / AOD / PWR_OFF), input drain, pacing, scene record, damage, stripe loop, drain.
- Runs in: UI task (main), core 0, prio 1. Optional `frame_priority 3` raises it during paint (default off; firmware/main/nos_frame_priority.h).
- Cadence: ON: paced to present fps (default 60 cap, authored 40, floor 30). AOD: 1 Hz render. PWR_OFF: 50 Hz loop (vTaskDelay 20) with `nos_bg_tick` at 1 Hz and CPU at 80 MHz.
- Reads: touch ring, settings fps_cap, power lock state, `nos_frame_pacer_capped`, flush credits.
- Writes: stripe buffers (indirectly), flush queue, DFS lock state.
- Files: firmware/main/nos_main.c:1899-2291 (loop :1977, PWR_OFF :2091-2103, AOD :2106-2130, ON path :2132-2290), nos/nos.c:2299-2509 (`frame_run`), firmware/main/nos_frame_wait.c, firmware/main/nos_frame_pacer.h.

### Scene draw and damage (eng-render)
- Purpose: `scene_draw` runs the app/shell draw list in either record mode (build `gfx_rec`) or raster mode (draw into a stripe fb); `compute_damage` diffs recordings; `render_rect` rasterizes damaged stripes.
- Runs in: UI task. Cadence: once per frame record, plus once per dirty stripe (up to 30x per frame for full-screen change).
- Reads: UI state, layer cache, fonts, sprites. Writes: stripe buffer in internal DRAM, `gfx_rec`.
- Files: nos/nos.c:1101-1145 (`draw_screen`), :1784 (`scene_draw`), :1942 (`compute_damage`), :2044 (`render_rect`), nos/render_bounds.h (`nos_glass_span`, round clip), nos/render_config.h (NOS_STRIPE_ROWS 16, NOS_DMA_ROWS 8).

### Band damage (eng-band-damage)
- Purpose: optional finer damage: 30 bands x 466 px, column spans (2-px granularity), max 2 spans per band.
- Files: nos/band_damage.c/.h (BD_ROWS 16, BD_BANDS 30, BD_MAX_SPANS 2). Cadence: per frame when enabled. Status: partial (optional path).

### Layer cache (eng-render-layers)
- Purpose: cache static full-screen layers as PSRAM rasters; 16-row bands validated by command hash.
- Resources: up to 3 slots x 434,312 B PSRAM via `hal_big_alloc`; admission floor 512 KiB.
- Files: nos/render_layers.c (SLOTS 3, ROWS 16; `allocate_layer` :46, `prepare` :102, `ensure_band` :173, `nos_layers_draw` :206).

### Blur stack (eng-blur) - sim-only on device
- Purpose: motion blur (mblur kernels, nos/nos.c:1246-1563), exposure/history (nos/render_exposure.c, render_history.c), ui_blur directional cache (nos/ui_blur.c, blur_cache.h, 2.6 MiB payload + 32 KiB header), pblur (7 slots of 466x233 indexed pages), kblur (used by sys_notifs, 896 KiB arena).
- On device with SHARP=1: all gated off (ui_blur.c:160, :292, :322, :335, :449; render_history.c:30; nos/nos.c:33-36, :2301; firmware/main/nos_main.c:459 `stripe_exposure_prepare` returns 0). Status: working in sim, dead on device, except kblur arena which is still allocated by `sys_notifs_prewarm` (see gaps).

### Display flush (eng-display-flush) and flush task (task-flush)
- Purpose: stripe rotation (`board_pixels_ccw_swap`, firmware/main/board_protocol.c:77), DMA buffers, QSPI draw_bitmap, credit system.
- Runs in: task-flush, core 1, prio 4, queue depth 3. Semaphores `s_buf_free` and `s_dma_ready` (given from DMA-done ISR `on_dma_done` :279). `JOB_FENCE` and `frame_drain` wait for completion; 1500 ms timeouts call `display_fault` -> `esp_restart()` (DIAG_DISPLAY_FAULT).
- Reads: stripe buffers (DRAM). Writes: 2 x 8-row DMA buffers, panel.
- Files: firmware/main/nos_main.c:118-130 (buffers), :133 (`display_fault`), :289-373 (`flush_task`; rotation :346, draw_bitmap :366), :433-456 (`stripe_wait`/`stripe_flush`/`frame_drain`), firmware/main/nos_panel_ack.h.

### Panel init and bus (hw-co5300-panel)
- Purpose: CO5300 init profile table (not a confirmed controller identity; SH8601 forced black), QSPI bus at LCD_SPI_HZ 80 MHz, gap X=6, LCD_EN GPIO42, RST 21.
- Files: firmware/main/nos_main.c:194-277 (`panel_execute`, `panel_command`, `co5300_init`), :394-430 (`lcd_init`; probe :402, bus :404), firmware/main/pins.h, firmware/main/lcd_probe.c (boot probe; result unused).

### TE sync (drv-display-te)
- firmware/main/nos_display_te.c: no-op on B (GPIO8 is IMU interrupt). On C waits up to 25 ms (not applicable to B).

### Touch task (task-touch)
- core 1, prio 5; FT3168 poll 10 ms active / 40 ms idle; 16-entry ring consumed by the UI task; coordinates mapped (465-y, x) for the CCW display. firmware/main/nos_main.c:581.

### Command task (task-cmd)
- prio 4 serial/command handler created beside flush and touch in `boot_resources_init` (firmware/main/nos_main.c:1870-1897). Shares core scheduling with the render path via the capture/probe hooks only when diagnostic commands are issued.

### Dot-matrix renderer (eng-dot) and dot bake (task-dotbake)
- Purpose: dot-style digit/glyph rendering with lazily baked PSRAM masks (~550 KB, q up to 160) and a ring circle cache (nos/dot.c:740-760).
- Runs in: UI task on demand; `dotbake_task` (core 1) pre-bakes at boot (firmware/main/nos_main.c:493; nos/dot.c:100-210, `dot_prebake_begin/all`), holding the "nos-prewarm" PM lock.
- Files: nos/dot.c, nos/dot_glyphs.c.

### Arc text (eng-ui-arc)
- Purpose: exact retained 32x32 glyph masks for curved text at rest, fixed-point atlas sampling in motion; 384 slots, 256-bucket hash.
- Memory: PSRAM via `nos_mem_alloc(..., "ui_arc")`, self-capped at 448 KB (nos/ui_arc.c:~40-56). Built up to 24 per frame (ARC_BUILD_LIMIT). Files: nos/ui_arc.c, nos/ui_arc_trig.h.

### PSRAM allocator and budget (eng-psram)
- `hal_big_alloc` is PSRAM-only (firmware/main/hal_esp32.c:1594-1597); under NOS_PSRAM_TRACKING it routes to `nos_mem_alloc` with a 6656 KiB budget, 64 owners, 1 MiB PSRAM free floor. Helpers `nos_mem_card_prepare`/`face_prepare`/`idle`. Files: nos/psram.c, core/psram.h.
- Many modules bypass accounting with raw `heap_caps_malloc/calloc` (voice, sports, workout, BLE, scan cache, native menu unpack arena at nos/native_menu_assets.c:321771-321776, list profile).

### Heap diagnostics (svc-heap-diag)
- firmware/main/nos_heap_diag.c: allocation-failure hook, `--wrap` of heap_caps_*, RTC_NOINIT failure history (8 slots), crash/coredump reader. Runs on every allocation (wrapper cost) and has a periodic tick even with screen off.

### Native menu assets (eng-native-menu-assets)
- Purpose: immutable grayscale menu art. `native_menu_assets_init` (called from nos_init, nos/nos.c:2093) inflates a packed blob (176,413 B packed) into a PSRAM arena (NATIVE_UNPACKED_BYTES), verifies per-asset hashes, falls back to dynamic artwork on failure. The 5.8 MB `native_asset_blob` const is flash-resident ROM. Skimmed only. Files: nos/native_menu_assets.c/.h.

### Fonts, sprites, gfx primitives (eng-gfx)
- core/gfx.c (fb primitives and the record/raster mode switch), core/font.c (stb_truetype glyph atlases), core/sprite.c (sprite8 indexed/palette blits), core/gfx_cost.h (cost counters). Run in UI task.

### Asset job and asset-read task (task-asset-read)
- core/asset_job.c state machine IDLE->QUEUED->RUNNING->DONE with cancel; `asset-read` task lazily created, prio 1, no core affinity, 6144 B PSRAM stack, reads SD in 32 KiB slices with vTaskDelay(1). firmware/main/hal_esp32.c:2447-2542. Competes with the UI task at equal priority.

### Power hooks (svc-render-power)
- firmware/main/nos_power.c, `go_state`/`cpu_mode` (nos_main.c:634-707): esp_pm DFS 80-240 MHz, "nos-ui" lock held while the screen is active, "nos-prewarm" for dotbake; no light sleep.

### Diagnostic captures and bench (tool-render-diag)
- nos_*_capture/probe headers, firmware/main/display_perf.c/.h and display_bench.c/.h (dead; only legacy main.c uses them), lcd_probe.c.

## How it runs

Frame path (ON state, per paced frame):
1. UI task wakes via completion-aware pacer (`nos_frame_pacer_capped`), an esp_timer one-shot "pace", and `nos_frame_wait_until`. Elapsed time is clamped to 4x the 40 Hz period.
2. Drain touch ring, run app update, then `scene_draw` in record mode -> `gfx_rec`.
3. `compute_damage` vs previous recording -> merged rects (gap 12, even-aligned) -> 16-row stripes (optionally band_damage spans).
4. For each dirty stripe: wait for a free stripe buffer (`stripe_wait`), `render_rect` re-runs the scene in raster mode into the DRAM stripe fb, with layer cache bands blitted from PSRAM, then `stripe_flush` enqueues it (queue depth 3).
5. Flush task (core 1, prio 4) rotates CCW + byte-swaps into 8-row DMA buffers, `esp_lcd_panel_draw_bitmap` over QSPI at 80 MHz; DMA-done ISR gives `s_dma_ready`; `s_buf_free` returns the stripe buffer.
6. `frame_drain` waits for the final DMA before sleeping / state changes. 1500 ms stall -> display_fault -> restart.

Scheduling and cores: UI core 0 prio 1; flush core 1 prio 4; touch core 1 prio 5; cmd prio 4; dotbake core 1 (boot only); asset-read prio 1 no affinity. On core 0 the UI shares time with esp_timer (22), NimBLE host (21), Wi-Fi task and lwip (18); any BLE/Wi-Fi burst preempts a frame mid-raster. INT_WDT 800 ms, TASK_WDT 5 s. FreeRTOS tick 1000 Hz, main task core 0.

Memory split: stripe fbs x2 and DMA buffers x2 (8 rows each) in internal DRAM (allocated at nos_main.c:1906-1911); all large persistent rasters, masks, scratch in PSRAM (octal 80 MHz). `SPIRAM_MALLOC_ALWAYSINTERNAL` 4096 keeps small mallocs internal.

Screen-off behaviour: PWR_OFF runs a 50 Hz `vTaskDelay(20)` loop at 80 MHz with `nos_bg_tick` 1 Hz; no rendering, no flush; AOD renders at 1 Hz (full scene record + damage). Idle prewarm of cards/faces (sys_home) is meant for the awake idle state.

Sharp-only device: the sim's 30 fps with motion blur budget is not what ships; the device does sharp raster with per-stripe re-run of the draw list.

## Data in / data out

| Direction | Data | From / To | Mechanism |
|---|---|---|---|
| in | touch points | task-touch -> UI task | 16-entry ring |
| in | app/shell UI state | Shell and faces, Utility apps | scene draw callbacks |
| in | fps_cap, brightness, screen timeouts | Settings and developer | settings getters |
| in | fonts, sprites, native menu art | flash/ROM + PSRAM arena | direct pointers |
| in | SD asset bytes | task-asset-read | asset_job state machine |
| out | pixels | task-flush -> CO5300 | QSPI DMA 80 MHz |
| out | display commands (brightness, on/off) | UI task -> panel | `panel_command` |
| out | DFS/"nos-ui"/"nos-prewarm" locks | Power and battery | esp_pm |
| out | DIAG_DISPLAY_FAULT then restart | display_fault | esp_restart |
| out | alloc failure history | nos_heap_diag -> RTC_NOINIT | 8 slots |

## Persistence

The renderer persists nothing to flash/SD/NVS. All caches (layer rasters, dot masks, arc masks, kblur arena, native unpack arena) are RAM/PSRAM and rebuilt on boot. Only RTC_NOINIT alloc-failure history (nos_heap_diag.c) and the fps_cap/brightness user prefs it consumes (owned by [[Settings and developer]] and [[Storage and persistence]]) survive reboot. `native_asset_blob` is flash-resident ROM from build time.

## Interactions

- [[Engine and scheduling]]: owns tick, pacing, and task creation; render is its heaviest client.
- [[Shell and faces]]: supplies scene draw lists, layer prepares, face/card prewarm calls.
- [[Utility apps]]: apps draw via gfx/ui primitives and cost counters.
- [[Settings and developer]]: fps_cap, diagnostics toggles, captures.
- [[Power and battery]]: DFS locks, screen-off loop, AOD cadence.
- [[Storage and persistence]]: asset-read SD slices, settings source of fps/brightness.
- [[Hardware and drivers]]: CO5300 panel, QSPI bus, touch FT3168, GPIO pins.
- [[Services hub]]: Wi-Fi/BLE/timer tasks that outrank the UI on core 0; PSRAM budget competitors.
- [[Connectivity and voice]]: raw heap_caps users that bypass the PSRAM budget.
- [[Health and activity]] / [[Sports and workouts]] / [[Calorie trace]]: draw clients; raw allocations in sports/workout.
- [[Gaps]], [[OS overview]]: cross-system findings and context.

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| high | UI task prio 1 on core 0 beneath Wi-Fi/BLE/timer; paints are preempted | firmware/main/hal_esp32.c:1531 (nimble_host 21); ESP_TASK_MAIN_PRIO; nos_main.c:1977 | Frame-time jitter during sync/scan |
| medium | Notification kblur arena (896 KiB) baked though SHARP makes `band_lv` 0 | nos/apps/sys_notifs.c:448-560 | Wasted PSRAM and CPU, memory pressure |
| medium | Home idle prewarm round-robin runs every frame | nos/apps/sys_home.c:632-650, :920-940 | Unneeded CPU/energy while awake |
| medium | Many raw heap_caps allocations bypass the 6656 KiB budget | hal_esp32.c:1594-1597; native_menu_assets.c:321771 | PSRAM floor can be breached without accounting |
| medium | Sim default SHARP=0 vs device SHARP=1: blur reviewed in sim never ships | firmware/main/CMakeLists.txt:73; nos/render_config.h | Misleading visual/perf expectations |
| low | Dead/compiled display_perf, display_bench; lcd_probe result unused | display_perf.c, display_bench.c, lcd_probe.c:402 | Flash/maintenance bloat |
| low | nos_heap_diag tick and wrappers run with screen off | nos_heap_diag.c | Small wake-up energy cost |
| low | No TE sync on B; tearing possible during stripe flush | nos_display_te.c | Visible tearing on fast motion |
