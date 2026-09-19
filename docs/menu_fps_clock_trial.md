# Menu FPS and 80 MHz QSPI trial — 2026-09-19

**Rejected after owner visual review.** The owner reported identical perceived
frame rate and noticeably worse performance in the left circular menu at
80 MHz. The exact visual failure mode was not specified; do not label it
electrical corruption or tearing without more evidence. The saved 40 MHz
CO-profile firmware and corresponding source were restored. The measurements
below are retained as evidence of why bus throughput alone was insufficient.

The owner requested a higher display clock and investigation of
[Civic Boost Gauge](https://github.com/kytos22/esp32-s3-civic-boost-gauge).
The CO5300 initialization and six-pixel offset remain selected. Only QSPI
frequency and clock diagnostics changed; menu rendering was not changed.
The application was flashed and hash-verified on the identified COM3 watch.

## Measurement

| Workload | 40 MHz | 80 MHz |
| --- | ---: | ---: |
| Full-screen SRAM, writes/s | 39.02 | 68.34 |
| Full-screen SRAM, payload MB/s | 16.95 | 29.68 |
| Full-screen generated pattern, writes/s | 24.22 | 33.02 |
| Circular menu, presentations/s | 18.47 | 18.78 |
| List menu, presentations/s | 50.47 | 50.48 |
| Circular menu mean raster, ms | 42.08 | 41.17 |
| List menu mean raster, ms | 12.81 | 13.17 |
| Circular menu frame-work P95 bound, ms | 64 | 64 |
| List menu frame-work P95 bound, ms | 24 | 27 |

Synthetic runs used 100 complete 466x466 RGB565 frames each. These are host
writes, not measured panel refresh rates. Each menu trial is one 12-second
trace including opening the overlay, ten alternating injected swipes and
settling time. Both clocks used the same replay script, with radios running.
These are exploratory comparisons, not statistically established speedups.
Trace names are `current`: the trace starts from home, then the host opens
the overlay and sends gestures while collection remains active. The replay
records which overlay each trace exercises. P95 values are histogram bounds.
All eight records have zero cancelled/pending transfers, unmatched completions
and submission overlaps; synthetic runs also returned ESP_OK.

The circular menu remains strongly limited by raster work. The list gets
essentially no measured FPS gain; its frame-work tail did not improve.
Bus improvements alone do not imply improved interactive menu performance.
The trial failed physical acceptance; no long-duration 80 MHz soak was run.
The previously reported fade defect is not fixed by this change.

ESP-IDF's ESP32-S3 SPI divider uses an 80 MHz APB source here. A requested
50 MHz resolves to 40 MHz, while 80 MHz uses the source directly. Diagnostics
now calculate the divider result instead of hardcoding 40 MHz. This is not
an oscilloscope measurement. `spi_get_actual_clock` is deprecated in IDF 5.5
but retained here because esp_lcd hides the SPI device handle required by
its recommended replacement; the build emits deprecation warnings.

Raw evidence is under `display-data/clock40-comparison.jsonl`,
`clock80-comparison.jsonl`, `menu-clock40.jsonl`, and `menu-clock80.jsonl`.
The replay script is `display-data/menu-clock-bench.py`; it imports repository
tools and is intended to be run from the repository root. Local build/flash
logs and the original replay are under `build/display-optimization/`.

## Useful reference techniques

Reviewed reference commit `9ca43404304388c5f7f489296d08e9e89c59f64a`.
Its [display setup](https://github.com/kytos22/esp32-s3-civic-boost-gauge/blob/9ca43404304388c5f7f489296d08e9e89c59f64a/src/main.cpp#L4785)
requests 80 MHz. Its custom gauge renderer uses cached states and dirty tiles,
not general-purpose full redraws at 60 FPS. The main application cache expands
to 4,899,092 bytes, in addition to two full PSRAM frames. Copying that approach
literally would consume too much of our working memory for a specialized gauge.

Ranked independently implementable changes:

1. Cache each menu's geometry once per frame: positions, scale, focus, bounds,
   and label widths. `wos.c` invokes menu draw functions for every dirty stripe;
   `app_launcher.c` and `app_applist.c` currently repeat substantial layout math.
   Reuse the same geometry for damage and raster passes to preserve consistency.
2. Cache compact icon/disc masks and curved labels. Prefer small primitive
   caches over pre-rendering all possible scroll/rotation states. Any scale
   quantization needs visual validation to avoid introducing stepping.
3. Profile transaction overhead before trying the reference's 16x16 dirty tile
   coalescing. Our menus already use per-element old/new damage, offscreen
   culling and separated label updates; basic dirty rectangles are not new work.
4. Consider pre-oriented cached assets to reduce rotation work after measuring
   the preceding changes. Our current rotation stage is not the dominant cost
   in the circular menu, so a renderer-wide orientation rewrite is lower priority.

Reference implementation anchors: `src/main.cpp` lines 279-308 (state/tile
constants), 2059-2161 (cache loading), 2605-2649 (tile flush), 2652-2738
(cached rendering). The app has no root application license at the reviewed
commit; no source, assets or generated caches were copied into our firmware.

## Rollback

The previous 40 MHz CO-profile application is preserved at
`build/display-optimization/co5300-40mhz-image/watchfw.bin`.
Application-only flashing at offset `0x10000` preserves NVS and partitions.
Source rollback is changing `LCD_SPI_HZ` in `firmware/main/pins.h` to 40 MHz
and rebuilding. The SH profile remains disabled. The 80 MHz clock-diagnostic
changes (including the deprecated API calls) were also reverted when restoring
the matching 40 MHz source and binary.
