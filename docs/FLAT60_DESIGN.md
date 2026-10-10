# Flat60 — a watch interface that always renders at 60 fps

Branch `60-fps`. Target: 1.43C (CO5300, 466×466 round AMOLED, 40 MHz QSPI,
ESP32-S3 at 240 MHz). Every number below was measured on the watch.

## 1. The budget

A frame is **16.7 ms**. Two independent pipelines must both fit:

**Bus (core 1 + DMA).** The panel link moves ~18.6 MB/s effective.

| Wire format | Bytes/px | Max px per frame @60 | Share of screen |
|---|---|---|---|
| RGB565 | 2 | ~155k (≈130k with margin) | 60–70% |
| RGB332 (fast display) | 1 | ~310k | > 100% (full screen is 217k) |

**CPU (core 0).** Measured per covered pixel, drawn in 16-row stripes as the
UI renders (`gfxbench` serial command, `core/gfx_bench.c`):

| Primitive | ns/px | Class |
|---|---|---|
| clear, fill_rect | 4 | free |
| circle (AA, span filled) | 25 | cheap |
| pill (AA) | 32 | cheap |
| blit from PSRAM sprite | 33 | cheap |
| 8-bit mask blit (icon cache) | 131 | moderate |
| big digits (glyphs) | 146 | moderate |
| alpha blend | 190 | expensive |
| text, ring | 260–320 | expensive |
| arc | 507 | expensive |
| vector icon | 641 | expensive |
| line (AA, banded) | 723 | expensive |

Fixed per-frame overhead (record pass, damage diff, flush bookkeeping) is
~1.5–5 ms, which leaves **~10 ms of raster** per frame.

## 2. Rules

1. **Only fills and sprite blits move.** Anything anti-aliased (shapes, text,
   icons, arcs, lines) is drawn once into a PSRAM sprite and then blitted.
   Moving 217k px of blits costs ~7 ms: the whole screen can move.
2. **No translucency in motion.** Dimming uses precomputed palette shades, not
   alpha blending.
3. **Flat RGB332-exact palette.** Every fill colour is exactly representable in
   RGB332, so the interface runs with fast display on (RGB332 wire format) and
   looks identical. Only anti-aliased edges take intermediate shades. This is
   what doubles the bus budget.
4. **Rigid motion.** Elements translate; they do not scale or rotate while
   moving. A scaled or rotated look is a separate pre-rendered sprite.
5. **Bounded damage.** Each moving element's dirty area is its old ∪ new box,
   so cost per frame is bounded by layout, never by gesture speed.
6. **True black background.** Clearing is free and OLED pixels are off.
7. **Cache misses are amortized.** Sprites render at boot or at most one new
   sprite per frame, never a burst inside a gesture.
8. **Transitions repaint only what changes.** The wipe crossfade repaints the
   band its front swept (~50k px/frame), not the screen.

## 3. Verification

- Device: `build/lite-ui/survey.py` traces late frames and p95 frame work per
  screen; `gfxbench` re-measures primitives; `dirty` prints the last frame's
  damage rects.
- Sim: `--txcheck` and `--facecheck` compare damage-tracked rendering against a
  full repaint frame by frame; any difference is a stale-pixel bug.
- Primitive changes must stay bit-exact (`build/lite-ui/prim_dump.c` compares
  old and new `gfx.c` over thousands of random shapes).
