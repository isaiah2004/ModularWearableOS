---
title: "Uncovered built files: blur pipeline, orrery shade, fonts"
tags: [nos, os-map]
updated: 2026-10-23
---

# Uncovered built files: blur pipeline, orrery shade, fonts

## Summary

Read in full: `nos/mblur.c`, `nos/blur_cache.c`, `nos/pblur.c`, `nos/kblur.c`, `nos/ui_blur.c`, `nos/render_layers.c`, `nos/faces/orrery_shade_q16.c`, plus the call sites in `nos/nos.c`, `nos/apps/sys_home.c`, `sys_control.c`, `sys_notifs.c`, `nos/faces/orrery_card.c`, `firmware/main/nos_main.c` and `firmware/main/nos_mblur_bench.h`. The `nos/fonts/nf_*.c` files are generated glyph tables, so only flash size, role and storage were recorded.

1. **The whole blur stack is inert on the 1.43-B build.** `firmware/main/CMakeLists.txt:73` defines `NOS_RENDER_SHARP=1`. `nos/nos.c:33-36` then defaults `nos_mblur` to 0, and `frame_run` forces `nos_mblur = nos_temporal_blur = nos_iris_blur = 0` on every frame (`nos/nos.c:2301`), so no USB command can re-enable blur on the device. Consequences: `nos_mblur_step()` returns 0, so `mb_h`/`mb_v` never run; `pblur_tick`/`pblur_draw` return 0 (`nos/pblur.c:149,225`); `ui_fblur_tick`, `ui_fblur_level*` and `ui_fblur_coverage_*` return early (`nos/ui_blur.c:160,292,322,335,449`); `native_tile.h` takes a plain `sprite8_blit` path. The simulator does not define SHARP (default 0, `nos/render_config.h:21`) and can cycle `nos_mblur` (sim/nos_sim.c `mblur` key, `B_MBLUR` button), so it shows blur the watch never renders. The code is still compiled (`GLOB_RECURSE nos/*.c`, CMakeLists.txt:15).
2. **Two parts are not SHARP-gated and still run on the watch:** `render_layers.c` (the retained screen raster cache, really used) and `kblur` (notification-card blur: the bake runs and allocates PSRAM, but `kblur_draw` is unreachable because `band_lv` is always 0).
3. **Variant selection when blur is enabled (SHARP=0):** horizontal slides use `nos_mblur_h` (box blur); vertical sheet/page/banner moves use `nos_mblur_v*` (per-column exponential smear); the Control drawer and the home pager use pre-baked `pblur` pages; scrolling lists use the `blur_cache` (shutters 2/8/16/40 px); notification cards use `kblur`.
4. **Orrery shade:** `orrery_shade_q16.c` builds and validates, once per boot in `dotbake_task`, 512 fixed-point multipliers plus a 512-byte lighting LUT as an immutable 4,608 B PSRAM block. `orrery_card.c` uses it for day-side shading. The `card_shade`, `card_qz`, `card_light` commands toggle variants and their `*_check` commands prove bit-identity. Live on the watch.
5. **Fonts:** six `nf_*` fonts, 280,856 B total in flash `.rodata`, read in place (XIP); only the SEGMENT face copies a scaled subset into PSRAM.

## Components

### eng-blur-kernels - nos/mblur.c
- Allocation-free kernels on RGB565 stripes in the expanded `0x07E0F81F` three-lane form (`expand`/`pack`, :4-6).
  - `mb_h4` (:32-47) with `h4_span` (:10-26): quarter-width point sample, box filter, linear reconstruction of 4 output pixels ((3a+b)/4, (a+b)/2, (a+3b)/4).
  - `mb_h4_rows` (:52-70): one filtered row shared by a row pair; a pending-row carry (`mb_h4_state`) makes 1-row and 16-row stripes give identical pixels.
  - `mb_v4` (:78-124): causal quarter-height exponential smear shared by 4 columns. Used only by pblur's vertical bake (k=3).
  - `mb_v_columns` (:126-230): full-column exponential moving average, k=1..4, `sum -= (sum&hi)>>k; sum += v<<(5-k)`, noinline first/continue loops per k, per-column `next` continuity tags (`mb_v_columns_state`: sum[466] u32 + next[466] u16). The only vertical kernel the live path uses.
- Runs on the caller's task (UI task) inside 16-row stripes or a pblur bake slice. Dead on the watch (`nos_mblur=0`); only `mblur_bench` exercises it. Tests: tests/mblur_test.c, mblur_columns_test.c, mblur_modes_test.c, mblur_gate_test.c.

### eng-blur-live-gate - live motion blur in nos.c
- `nos_mblur_step()` (nos.c:1315) = |step| / (30 * nos_dt), 0 when off or when held and `nos_mblur<2`.
- `mb_len_log2` (:1333): step >=64 -> 32 px box (k=5), >=32 -> 16, >=16 -> 8, >=8 -> 4, else none. `mb_h` (:1352): mode 1 full scalar sliding box; mode 2 `mb_h4_rows` with k clamped to 4. `mb_v` (:1513): tau = step/4, k = tau>=11 ? 4 : >=5.5 ? 3 : >=2.8 ? 2 : 1, always `mb_v_columns`.
- Admission `mb_region_ok` (:1446): base (last raster minus wait minus last blur) + record_us + 1 ms + already committed + cost must fit `NOS_MBLUR_BUDGET_US` = 26,000 (nos.h:123). Results full, reduced, or skip. Seeds `s_mb_ns = {100, 150, 35, 100}` ns/px (H full, V full, H reduced, V reduced); the H seeds are noted as measured on the 1.43-B, the V seeds are marked UNMEASURED (:1441-1444). `mb_learn` (:1496) is a 0.7/0.3 EMA that jumps up immediately. Whole-screen cost at the seeds (217,156 px): H full 21.7 ms, H reduced 7.6 ms, V full 32.6 ms (never fits), V reduced 21.7 ms. Fallback base 18 ms (19 ms for areas over half a screen).
- Scratch `mb_work` (line[GFX_W+40], 2 h4 states, 2 vertical states) is a PSRAM block allocated by `hal_big_alloc` on first use (:1256-1271).
- In record mode a blur emits one whole-region command whose hash includes `nos_frame` and bumps `s_mb_record_epoch`; `render_layers` bypasses its cache when the epoch changes.
- `nos_mblur_afford()` (:1306) is the iris-only guard.
- Counters: `nos_mblur_count[2][2]`, `nos_mblur_reduced[2]`, `nos_mblur_gate`, `nos_blur_stat`; printed by `info` (nos_main.c:1712-1727). All zero on the watch.

Which variant serves which move (scene_draw, nos.c:1784-1840, and screens):

| Move | Call | Variant |
|---|---|---|
| TX_PUSH slide | `nos_mblur_h` on under (0.30 x step) and over (1 x step) layers | H box, full or reduced by admission |
| TX_RISE/DROP, Control drawer (H*mbs >= 8) | `pblur_draw(id 6)` | baked vertical page |
| TX_RISE/DROP, Control drawer otherwise | `nos_mblur_v_reduced` | reduced V |
| TX_RISE/DROP, other sheets | `nos_mblur_v` | V full |
| Banner card drop (:1773-1778) | `nos_mblur_v_rect` | V columns |
| Home pager (sys_home.c:960-975) | `pblur_draw` ids 0..5, else `draw_page` + `nos_mblur_h_reduced` when mbs >= 8 | baked H page else live reduced H |
| fit_activity, fit_heart, out_weather pages | `nos_mblur_v` | V |
| out_baro, out_nav pages | `nos_mblur_h` | H |
| Lists (ui.c:707, set_modules, set_diag, set_about, time_alarm, time_stopwatch, out_weather) | `ui_fblur_*` | blur_cache |
| Notification cards | kblur, live `nos_mblur_v_fallback` on a miss | kblur / reduced V |
| Iris open/close | own `iris_draw`/`iris_blend`, gated by `nos_iris_blur && nos_mblur_afford()` | not mblur.c |

### eng-blur-ui-cache - ui_blur.c + blur_cache.c (list blur cache, PSRAM)
- **Memory:** `BC_BYTES` 2,600 KiB (2,662,400 B) = `BC_HEADER` 32 KiB + 642 blocks of `BC_BLOCK` 4,096 B; `BC_SLOTS` 48; `BC_SLICE` 8 rows (blur_cache.h:6-12). The header (`fb_storage`: cache state, `gfx_rec`, 128 coverage rects, 32 exclusions) is allocated first by `hal_big_alloc`; the payload is sized once by `fb_payload_target` between `BC_DEFAULT_BYTES` 1 MiB (1,015,808 as a minimum payload) and 2,629,632 B, capped by free PSRAM minus 512 KiB under `NOS_PSRAM_TRACKING`.
- **Keying:** item = (owner pointer, item index, w, h, context, x0, horiz) (`blur_cache_find`, blur_cache.c:69). Content identity is `fb_sig`, a hash of the recorded commands in the item's band (ui_blur.c:245); `fb_validate` (:274) checks lazily at most once per frame; `fb_capture_ready` (:356) waits for a changing source to settle. Each item owns a sharp range `w*(h+2C)*2` and a blur range `w*h*2` from the block map; one shared staging area; publication swaps ownership instead of copying.
- **Eviction:** `reserve_blocks` (blur_cache.c:76-96) is first-fit contiguous; on failure it evicts the least-recently-`touched` unpinned item and retries, returning -1 if only pinned items remain (pinned = visible, the nearest ahead and one behind). `ui_fblur_idle` frees the payload when the top screen is `SCR_NATIVE_CACHE`; `ui_fblur_release` frees all (called from `nos_mem_card_prepare`, psram.c:140-147, and set_settings.c:1235).
- **Levels:** lengths {2, 8, 16, 40} px. Retained ladder at >=15/150/720/1800 px/s gives levels 1..4. Public `ui_fblur_level` crosses at 360/840/1680 (0..3); the early ladder is 150/300/960/1920 (0..4). Modes by `bands&3`: 0 vertical, 1 vertical with 24-row context, 2 horizontal page; flags `UI_FBLUR_EARLY` 0x10, `UI_FBLUR_PREWARM` 0x20.
- **Cost per call:** `ui_fblur_tick` does at most 6 slices of 8 rows (`FB_WORK_SLICES`) and stops at 1,500 us on the device (the sim has no time limit). Filter is a sliding window, so cost is independent of length: packed three-lane u32 for lengths 2/8/16, wide B/R plus G lanes for /40 when `blur_cache_packed`, otherwise scalar `unsigned short sums[466][3]`. `blur_cache_packed` is an exact A/B switch.
- Also owns `nos_native_row_stage_mode` (ui_blur.c:15) and the 512 B `DRAM_ATTR nos_native_row_stage_scratch` (:20); coverage rects (:133-243) feed the compositor's `ui_fblur_row_spans`.
- **Status on the watch:** dormant (SHARP early-returns), so the payload is never allocated. The documented 2,662,400 B "UI blur bytes" figure comes from an older build with blur on (docs/OPTIMIZATIONS_DEVICE.md:102). Tests: tests/blur_cache_test.c, blur_cache_shared_stage_test.c, ui_blur_*_test.c.

### eng-blur-pblur - nos/pblur.c (persistent baked page blur)
- **Slots:** `PB_N` = 7. Ids 0..5 are home pages (0 face, 1 Activity, 2 Heart, 3 Weather, 4 Music, 5 Timer; page 1 is the Apps launcher and is skipped), id 6 is the Control drawer.
- **Format and memory:** 8-bit palette-index pages at half height, 466 x 233 = 108,578 B each (`PB_PIXELS`), allocated lazily per slot with `hal_big_alloc` and admission-gated (>=512 KiB must remain; failures retry after 30 frames), plus a stage page and a work struct of about 25,956 B. Maximum footprint about 894,580 B. Palette = 216-entry 6x6x6 cube + 40 neutral greys (pblur.c:73-81); drawn with `sprite8_blit_y2`.
- **Keying/validity:** slot id plus an FNV-style signature (start 2166136261) over the recorded commands' hash and bbox, obtained by running the page draw with `pblur_baking` set into a `gfx_rec`; an overflowing record yields sig 0 and is never published. A slot is valid for 3.0 s after its last check; re-signature at most every 2.0 s (`pblur_check`/`pblur_ready_mask`).
- **Eviction:** none. Fixed slot per page; explicit `pblur_invalidate(id)` (sys_home, sys_control) and `pblur_release()` (psram.c:140-147, set_settings.c:1235).
- **Bake cost:** `pblur_tick` runs idle only, once per frame, skipped if cpu + record_us >= 20,000 us (`pblur_why[3]`); up to 4 slices of 16 rows per tick (30 slices per page), stopping after 6,000 us on device; a job is abandoned after 8 s (`why[1]`) or when another id starts (`why[0]`); a signature mismatch at the end discards it (`why[2]`). Vertical pages use `mb_v4` k=3, horizontal pages `mb_h4_rows` k=4, then palette-convert every second row.
- **Draw:** `pblur_draw` needs step >= 8; counters `pblur_hits[2]`, `pblur_misses[2]` (axis index), published, discarded.
- **Driver:** `home_blur_tick` from sys_home.c:640 on even idle frames cycling ids 0..5, then `sys_control_prewarm` for id 6.
- **Status on the watch:** dead (both entry points return 0). Tests: tests/pblur_test.c.

### eng-blur-kblur - nos/kblur.c (notification-card blur cache)
- **Memory:** `kblur_new(w, max_h, ctx, bytes)`; sys_notifs.c uses arena 896 KiB, `KB_W` 347, `KB_MAXH` 189. Up to 32 slots; each holds 4 levels (vertical box 8/16/32/48 px, `KB_NLEV`=4) of 8-bit grey (green channel) at half height. About 6 slots of ~131,860 B fit (derived estimate); the CLEAR button keeps one, so the viewport window is slots-1.
- **Keying/eviction:** key = notification id (or `KB_CLEAR` 0xC1EA0001) + sig + h. A free slot first, else LRU by stamp. `kblur_keep_only` drops dismissed items (kblur.c:189).
- **Cost per call:** `kblur_bake(budget_us)`, 2,000 us from sys_notifs (sim: 6 fixed steps): capture in 16-row strips (`KB_STRIP`) into a canvas with 24 rows of context, then one blur level at a time. Never during transitions.
- **Runs:** UI task; `band_tick` when the list is still (sys_notifs.c:568) and Home on odd frames when `!face_interactive` and `nos_frame % 3 == 1` (sys_home.c:640-647). `nos_mem_face_prepare` releases it for interactive faces.
- **Status on the watch:** not gated by `nos_mblur`/SHARP, so it allocates and bakes whenever notifications exist; `kblur_draw` is never reached. Tests: tests/kblur_slice_test.c.

### eng-render-layers - nos/render_layers.c (retained screen rasters, live on the watch)
- **Memory:** `SLOTS` 3, each `GFX_W*GFX_H*2` = 434,312 B in PSRAM (3 x = 1,302,936 B), plus one `gfx_rec` (320 commands x 20 B + 8 = 6,408 B); total 1,309,344 B. Allocated lazily by `allocate_layer` (:46) only when an eligible screen draws. Under `NOS_PSRAM_TRACKING` it needs >= 512 KiB free afterwards, otherwise the live renderer is used; failure sets `allocation_blocked`.
- **Keying:** by `const nos_screen *` (`find_layer`, :82). Reuse a matching slot; otherwise victimise the LRU by `touched` frame, never one touched this frame; otherwise take an empty slot.
- **Validity:** 30 bands of 16 rows (`ROWS`, `BANDS`). `prepare` (:102) runs the screen draw once per frame into the recorder and hash-chains each command (hash + bbox) into the bands it overlaps. Bypass if the blur epoch changed (never on device), the recorder overflowed (320 commands), or observed changed rows x 2 >= 466. `ensure_band` (:173) re-rasters invalid bands, borrowing the output stripe buffer in internal RAM when the band is complete and untranslated, then memcpy to PSRAM; `nos_layers_draw` (:206) blits it.
- **Eligibility:** skipped for `fb->rec`, `SCR_NATIVE_CACHE` screens (home, launcher, notifications, settings, orrery card), `SCR_LIVE_RENDER` screens (sports, background jobs; nos.c:1126-1127), or `ui_fblur_any_active()`.
- **Lifecycle:** `nos_layers_idle` releases all rasters when a NATIVE_CACHE/LIVE_RENDER screen is on top and not transitioning (via `nos_mem_idle`, nos.c:2344); `nos_mem_card_prepare` also releases them. USB `layers 0|1` toggles `nos_layer_cache_enabled` (default 1) and invalidates.
- Tests: tests/nos_render_layers_test.c.

### eng-orrery-shade-q16 - nos/faces/orrery_shade_q16.c
- **Table:** `shade_cache` (:7-12), asserted 4,608 B: `float coefficients[512]` (planet .05+.95*L/255 for 0..255; sun .55+.45*L/255 for 256..511), `uint32_t multipliers[512]`, `uint8_t lighting[512]` (`round(255*powf(i/255, .85))` for the planet, exponent .6 for the sun). Allocated by `nos_mem_alloc(..., "card.shade_global")`, never freed, published read-only.
- **Derivation:** `derive_multiplier` (:37) tries floor(coef*65536) then +1 so that `(v*m)>>16` equals `(int)(v*coef)` for v=1..63 and `(v*m+32768)>>16` equals `(int)(v*coef+.5)` for v=1..31 (near-grey rounding). Any coefficient with no match rejects the table.
- **State machine:** atomic EMPTY -> BUILDING -> READY or REJECTED, single attempt, terminal. Rejection reasons: 1 admission (`nos_mem_available() < 4608+16384+8`), 2 allocation, 3 validation mismatches. For 1 and 2 it returns ~0u, which is ambiguous with "pending". `orrery_shade_q16_get(coeffs)` returns the multipliers only if the caller's 512 floats are bitwise identical (memcmp); `orrery_shade_lighting_get()` returns the LUT.
- **Runs:** `dotbake_task` ("dotbake", 3,072 B stack, priority 1, pinned to core 1; nos_main.c:493-507, created at :1950 only if `dot_prebake_begin()` succeeds): `dot_prebake_all`, `orrery_basis_check`, shade prebake, bracketed by `nos_power_bake_begin/end`, prints a `PREBAKE ... shade_mismatches= shade_ready= shade_rejection=` line, then ends. Once per boot, independent of screen state; a prior docs note recorded 813 ms for the whole prebake.

### eng-orrery-card-shade - consumers in nos/faces/orrery_card.c
- **Relation:** `shade()` (~:883) is the original float day-side shader (per-channel multiply by coefficient k; near-grey pixels round and rebuild G). `shade_q16(c, m)` (:896) is the integer replacement, used only when `night == 0 && m <= 65536` (:1349, :1433); night-blended pixels always use the float path.
- **Setup:** `allocate_profile_impl` (~:1873-1900) copies the prebaked LUT via `orrery_shade_lighting_get()` (else runs `powf`) and builds `C.lighting_coeff` (2 KiB `float k[512]`, orrery_card_lighting.h) which is bit-checked against the formula. `render_globe_profile_impl` (:1559-1592) fetches `C.shade_q16 = orrery_shade_q16_get(C.lighting_coeff->k)` once per card, then dispatches once per call to render_sun_exact_1_1 / legacy / `render_globe_fast_cached_q16` ... by `qz_exact`, `lighting_cache`, `shade_q16` and `sun_fast`; no mode test in the pixel loop.
- **QZ:** `card_qz_exact()` (orrery_card_qz.h) is a bit-exact integer reconstruction of `n/32767.0f`; `orrery_card_qz_check` (:1548) compares all 65,536 inputs bitwise.
- **Flags** (RAM only, default 1; :1509-1512): `orrery_card_qz_exact`, `orrery_card_lighting_cache`, `orrery_card_shade_q16`, `orrery_card_sun_fast`.

USB commands (cmd_task -> `s_cmd_q` -> UI task; nos_main.c:1456-1522):

| Command | Effect | Output |
|---|---|---|
| `card_qz 0\|1` | exact vs legacy division | `NOSDIAG card_qz=` |
| `card_qz_check` | all 65,536 inputs, no card needed | `CARD_QZ_CHECK cases=65536 mismatches= elapsed_us=` |
| `card_light 0\|1` | precomputed coefficient table on/off | `NOSDIAG card_light=` |
| `card_light_check` | 512 coefficients vs formula; also recomputes powf; card must be open | `CARD_LIGHT_CHECK cases=512 ...` |
| `card_shade 0\|1` | integer Q16 vs float shader | `NOSDIAG card_shade=` |
| `card_shade_check` | 512 coefficients x (64 + 32 + 32) test colours vs `shade()` | `CARD_SHADE_CHECK coefficients=512 mismatches= elapsed_us= ready= rejection=` |

The `*_check` commands print ~0u (4294967295) when the card is not open (no `lighting_coeff`). Toggling a flag does not dirty a static globe (docs/OPTIMIZATIONS_DEVICE.md:296). NOSSTATE shows `qz= light= shade= shade_ready=`. Tests: tests/orrery_shade_q16_test.c.

### tool-usb-blur-switches - USB commands touching this stack
All state is RAM only and resets at boot; no pref or NVS key exists for any blur setting.

| Command | Variable | Default | Effect on the 1.43-B |
|---|---|---|---|
| `cache_packed 0\|1` (nos_main.c:1397) | `blur_cache_packed` | 1 | none (filter never runs); prints `NOSCACHEMODE v=1 packed=N`; error during capture |
| `native_stage 0..3` (:1445) | `nos_native_row_stage_mode` (bit0 stage 128 px source chunks via the 512 B scratch, bit1 copy the 256-entry LUT locally) | 0 | none (native_tile.h:392 SHARP early return); prints `NOSDIAG native_stage=N`; error during capture or touch |
| `mblur_bench` (:1652) | runs `nos_mblur_bench_run()` | n/a | the only live use of the blur kernels |
| `layers 0\|1` (:1392) | `nos_layer_cache_enabled` | 1 | live |
| `history 0\|1`, `exposure_worker` | temporal blur | forced off under SHARP | none |
| `info`, `state` | counters (BLUR, MBLUR, PBLUR lines; NOSSTATE packed=, cache_packed=, native_stage=) | n/a | blur counters read zero |

There is no `mblur` command; only the simulator can change `nos_mblur`.

### tool-mblur-bench - firmware/main/nos_mblur_bench.h
Times `mb_v_columns` in isolation: 4 input patterns x k 1..4 x 2 passes over 466 x 466 in 4-row stripes (3,728 B internal stripe, 2,796 B state in PSRAM via raw `heap_caps`). Prints `NOSMBENCH_CELL ... ns_per_px_q8= ... cpu_mhz` and a summary `NOSMBENCH v=1 state=1 cells=16`. Runs synchronously in the UI task (estimated 0.3-1 s of blocking), is in the read-only and quiet command lists (nos_main.c:1810 area), and is rejected during capture.

### eng-fonts - nos/fonts/nf_*.c
- Generated by `tools/bake_font.py --nos` from Space Grotesk (variable wght) and Space Mono Regular (third_party/fonts). Each file holds `static const uint8_t` 8-bit alpha bitmaps, `font_glyph[]` and a `font_t` (glyphs, nglyphs, bitmap, line_height, ascent). `core/font.c` blits them (`find_glyph`: direct index, else binary search; a missing glyph advances line_height/3). It does not use stb_truetype.
- **Storage:** `const` data in flash `.rodata` (DROM 0x3c6ebe98..0x3c7307b5 in firmware/build/watchfw.map), read in place via the flash cache, not copied to PSRAM. Exception: `fk_sfont` (face_kit.h:163-166, faces.c:679-697, face_segment.c:206/:328) is a PSRAM copy of the `nf_hero` digits and ':' scaled by 0.66 (12 glyph slots, built one glyph per call).
- 97 glyphs = ASCII 32..126 plus 0xB0 (degree) and 0xB7 (middot); `nf_hero` has 11 (0-9 and ':'). Non-ASCII text is transliterated by `gb_ascii` (nos/gb.c, svc.c:108).

| Font | Spec | Role (nos/ui.c:34-39) | Bitmap B | Glyph table B | Flash total B |
|---|---|---|---|---|---|
| `nf_hero` | Space Grotesk Regular 160 px (205/158) | `UI_F_HERO`: full-screen clock digits (SEGMENT and SPLIT faces) | 93,116 | 132 | 93,264 |
| `nf_display` | Space Grotesk Medium 64 px (82/63) | `UI_F_DISPLAY`: large values (con_chat.c:236 volume, sys_background.c:130, faces.c:105 `fk_clock_fallback`) | 117,870 | 1,164 | 119,050 |
| `nf_title` | Space Grotesk Medium 32 px (42/32) | `UI_F_TITLE`: screen titles (50 refs, 13 files) | 28,765 | 1,164 | 29,945 |
| `nf_body` | Space Grotesk Regular 26 px (34/26) | `UI_F_BODY`: body text (89 refs, 23 files) | 18,663 | 1,164 | 19,843 |
| `nf_label` | Space Mono Regular 18 px (28/21), +0.06 em tracking | `UI_F_LABEL`: labels, buttons, list rows (88 refs, 28 files) | 9,200 | 1,164 | 10,380 |
| `nf_small` | Space Mono Regular 15 px (23/17), +0.07 em tracking | `UI_F_SMALL`: micro text (78 refs, 18 files) | 7,194 | 1,164 | 8,374 |
| Total | | | | | 280,856 (about 274 KiB, about 4.9% of the 0x575448 flash rodata) |

- Big dot-matrix numbers use `nos/dot.c` masks, not these fonts. The native menu ROM (native_menu_assets.c) is baked from `UI_F_BODY/LABEL/TITLE` and re-verified every build by `tools/generate_native_menu_assets.py --verify` (CMakeLists.txt:60-67). `NOS_FONT_SHIM` borrows `core/fonts` only if `nos/fonts/nos_fonts.h` is missing (CMakeLists.txt:79-84), so core fonts are not built. Header comments in nos/ui.h:75-80 give stale sizes (~30/24/17/14 px vs actual 32/26/18/15).

## How it runs

- **UI task (`app_main`, core 0):** record pass, stripe raster, layer cache, `ui_fblur_*`, `pblur_*`, `kblur_*`, `mb_*` and the USB command executor (dequeue at nos_main.c:1994) all run here. `cmd_task` ("cmd", 6,144 B stack, priority 4) only parses and queues.
- **Core 1:** only the one-shot `dotbake` task (priority 1) touches the orrery shade. No blur or cache code runs on core 1.
- **Idle bakes** are time-boxed: ui_blur 1.5 ms, kblur 2 ms, pblur 6 ms per call. Prewarm runs only while Home is on top, awake and not transitioning; `pblur_tick` refuses in AOD. With the screen off nothing in this note runs except the boot-time shade prebake.
- **SHARP=1 consequence:** `NOS_MBLUR_BUDGET_US` and the cost seeds are never consulted on the watch; they apply only to a SHARP=0 build.

## Data in / data out

| Direction | Data | Peer | Mechanism |
|---|---|---|---|
| in | screen draw callbacks and recorder (`gfx_rec`, 320 commands) | all screens | `screen->draw(&fb)` in record and raster mode |
| in | per-frame motion step | transition/list/pager code | `nos_mblur_step`, `ui_fblur_tick(step)` |
| in | `nos_last.raster_us/wait_us/record_us` | nos.c | globals for budgeting |
| in | notification list | svc_notif | `card_sig`, `kblur_bake` |
| in | 512 shade coefficients | orrery card | `orrery_shade_q16_get` |
| in | USB text commands | host | cmd_task -> queue -> UI task |
| out | blurred pixels | stripe buffer | kernels, `sprite8_blit_y2`, `sprite_blit` |
| out | coverage rects | compositor | `ui_fblur_coverage_claim/exclude`, `ui_fblur_row_spans` |
| out | Q16 multipliers and lighting LUT | orrery card | `orrery_shade_q16_get`, `orrery_shade_lighting_get` |
| out | telemetry | host | `info` BLUR/MBLUR/PBLUR, NOSSTATE, PREBAKE, `CARD_*_CHECK`, `NOSMBENCH*` |

## Persistence

Nothing here persists. Layer, ui_blur, pblur and kblur arenas are PSRAM and rebuilt on demand; the Q16 table is rebuilt every boot; diagnostic flags reset at boot. No NVS keys, SD files or prefs exist for blur or shade. Fonts are build-time flash data.

## Interactions

- [[Rendering pipeline]]: this note refines its blur and layer-cache sections.
- [[Engine and scheduling]]: UI task, frame loop, command queue, `nos_mem_idle`, `nos_mem_card_prepare`.
- [[Shell and faces]]: Home pager (pblur ids 0..5), SEGMENT/SPLIT faces (`nf_hero`), Orrery card (shade), notifications (kblur).
- [[Utility apps]]: Activity/Heart/Weather/Barometer/Navigate pages, Alarm, Stopwatch calling `nos_mblur_*` or `ui_fblur_*`.
- [[Settings and developer]]: Developer Tools commands, Settings trim (`ui_fblur_release`, `pblur_release`).
- [[Power and battery]]: `nos_power_bake_begin/end` around the prebake; idle-bake CPU cost.
- [[Storage and persistence]]: no persisted blur state.
- [[Hardware and drivers]], [[Services hub]], [[Health and activity]], [[Sports and workouts]], [[Connectivity and voice]], [[Calorie trace]]: no direct dependency on these files beyond the shared render path.
- [[Gaps]], [[OS overview]]: cross-system context.

## Gaps

| Severity | Gap | Evidence | User impact |
|---|---|---|---|
| medium | kblur arena (896 KiB, about 6 slots) is allocated and baked though `band_lv` is always 0, so it is never drawn; `kblur_bake` has no SHARP gate | nos/kblur.c:134; nos/apps/sys_notifs.c:492,560; sys_home.c:640-647 | wasted PSRAM and idle CPU/energy while notifications exist |
| medium | Large dead blur code still compiled; image is 7,837,568 B in a 7,864,320 B OTA slot (26,752 B spare) | firmware/main/CMakeLists.txt:15,73; firmware/build/watchfw.bin | little flash headroom; savings unmeasured (linker may drop unreferenced code) |
| medium | `cache_packed` and `native_stage` flip flags nothing reads on SHARP; NOSSTATE still prints them | nos_main.c:1397,1445,1677; native_tile.h:392 | misleading A/B bench results |
| low | `mblur_bench` blocks the UI task (estimated 0.3-1 s), uses raw `heap_caps` outside the PSRAM budget, allowed while display is off | nos_mblur_bench.h; nos_main.c:1652,1810 | UI stall during a bench |
| low | V-blur cost seeds are unmeasured and bench output is not fed back | nos/nos.c:1441-1445 | only matters if blur is re-enabled |
| low | `card_*_check` return 4294967295 when the card is not open; the prebake returns ~0u for both "pending" and admission/allocation rejection | orrery_card.c:1513-1540; orrery_shade_q16.c:51-96 | confusing diagnostics |
| low | Shade prebake is one attempt per boot and skipped if `dot_prebake_begin()` fails; fallback is float shader plus 512 `powf` per card entry | orrery_shade_q16.c:62-70; nos_main.c:1950 | slower card entry only |
| low | Home idle cycle still calls `pblur_tick` and `sys_control_prewarm` (tile signature) although blur is off | sys_home.c:640; sys_control.c:542-551 | small wasted CPU while Home idles |
| low | Stale font size comments in ui.h | nos/ui.h:75-80 | documentation drift |
| low | Sim default SHARP=0 means blur tests validate code that does not run on the watch | nos/render_config.h:21; CMakeLists.txt:73 | test coverage differs from device behaviour |
