# Dial lettering and Ultra background — 2026-09-19

The circular launcher now uses the supersampled Saira Condensed arc renderer
already used by the Samsung-inspired Ultra face. It replaces the old rotated
bitmap lettering while retaining the label's position and accent color.
Changing focus repaints the union of the outgoing and incoming label bounds,
so a shorter name cannot leave the previous name's ends behind.

The vector text is rendered once per focus/cache change into a retained alpha
patch. Stripe draws only iterate the patch rows and columns inside their clip.
The patch and staging buffer use at most 104,486 bytes of PSRAM and no full-screen
layer slot. Current labels fit within 67 by 80 pixels; capacity is 242 by 89.
An allocation or capacity failure falls back to direct drawing, preserving the
label at a higher rendering cost.

Ultra and Nothing previously shared background-cache tag 10. After cache
eviction, a revisit to Ultra with unchanged live values could accept Nothing's
cached sparse background. Ultra now has its own tag, 13. A native regression
renders Nothing after a cache tour and verifies Ultra's complete output against
its original pixels; restoring tag 10 makes the regression fail.

## Validation

- Full native suite and 21 Python tests pass (`tools/test.ps1`).
- Launcher tests retain 452 baseline geometry comparisons, excluding the
  intentionally changed lettering region. Separate checks cover exact current
  full-frame versus translated stripe output including all label pixels, one
  rasterization per cache miss, label capacity, and long-to-short dirty coverage.
- Simulator gesture-routing tests and ESP-IDF/simulator builds pass.
- Simulator previews show the complete Ultra background and replacement label.
- COM3 application-only flash was hash-verified; QSPI remains 40 MHz with the
  working CO initialization profile. The 80 MHz esptool flash-frequency argument
  configures program flash, not display QSPI.
- USB navigation passed 26 apps, 13 faces, four overlays and injected touch.
  Ultra (face 4) and the normal sleep policy were restored. After the face tour,
  telemetry reported 370,380 bytes of free PSRAM and 59,551 bytes of free internal
  RAM, with a 30,720-byte largest DMA-capable block.

The same 12-second-per-menu replay produced these results. They are single-run
measurements including entry, gestures and settling, not guaranteed steady FPS.

| Measurement | Previous rotary build | This build |
| --- | ---: | ---: |
| Dial presentations/s | 38.65 | 39.57 |
| Dial mean raster work, ms | 17.16 | 16.34 |
| Dial frame-work P95 upper bound, ms | 27 | 26 |
| List presentations/s | 53.00 | 51.38 |

The small list variation is not evidence of a list optimization; list rendering
was not changed. Both new traces have zero pending/cancelled transfers,
unmatched completions and submission overlaps.

Evidence: [menu replay](display-data/menu-label-ultra-replay.jsonl),
[navigation](display-data/label-ultra-navigation.jsonl),
[binary/source hashes](display-data/label-ultra-image.json).
The matching application binary, ELF, configuration and changed rendering source
are retained locally in `build/display-optimization/label-ultra-image/`;
the previous working image remains in `build/display-optimization/menu-final-image/`.

USB checks and simulator images do not establish physical display quality.
The owner should verify the lettering and Ultra background on the watch.
