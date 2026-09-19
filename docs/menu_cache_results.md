# Cached menus and rotary dial — 2026-09-19

The circular launcher repeatedly evaluated geometry and rotated glyph pixels
for every 16-row raster strip, even when those glyphs were outside the strip.
The optimization caches label glyph geometry and each frame's layout, clips
glyph loops before iterating pixels, and rejects fully off-strip discs before
calling their vector icon routines. The curved list similarly caches visible
row layouts and its focused label width once per frame. Colors, antialiasing,
layout formulas and explicit damage behavior are preserved; no palette reduction
or SD storage is involved. Static RAM usage increases by approximately 2.9 KB.

## Controlled performance comparison

Both measurements use 40 MHz QSPI, CO initialization, the same 12-second host
replay per menu, and the original horizontal gesture mapping. Each replay
includes overlay entry, ten alternating swipes and settling. These single-run
comparisons establish a substantial dial improvement, not guaranteed steady
60 FPS or a statistical characterization of all gestures.

| Measurement | Before | Cache-only |
| --- | ---: | ---: |
| Dial presentations/s | 18.40 | 40.22 |
| Dial mean raster work, ms | 41.89 | 15.61 |
| Dial frame-work P95 upper bound, ms | 64 | 25 |
| List presentations/s | 49.94 | 53.82 |
| List mean raster work, ms | 13.14 | 11.12 |
| List frame-work P95 upper bound, ms | 25 | 21 |

All four traces have zero cancelled/pending transfers, unmatched completions,
and overlapping submissions. The dial still misses some 16.7 ms deadlines.
The larger total transmitted pixel count after optimization comes from producing
more presentations during the same wall-clock interval, not a larger fixed
per-frame budget. The performance result is distinct from optical acceptance.

Evidence: [before](display-data/menu-before-cache.jsonl),
[cache-only](display-data/menu-cache-only.jsonl). Replay:
`python docs/display-data/menu-clock-bench.py <output.jsonl>` from repository root.
The cache-only image and matching source are preserved locally in
`build/display-optimization/menu-cache-only-image/`.

## Rotary interaction

The owner observed that the old dial used only horizontal finger displacement.
The final version instead computes the finger's angle about the center and
applies unwrapped angular differences directly to the group or app ring.
Moving along the same radial line does not spin it. A roughly 23-pixel center
deadzone invalidates the angle anchor; exiting reanchors without a jump.
Release velocity is normalized to a 16.67 ms interval, bounded, and cleared
after a pause exceeding 100 ms. Existing settling/snapping remains.

The OS now treats any already-claimed overlay drag as a drag on release,
including a circle returning to its starting point. Such gestures send
TOUCH_UP rather than an accidental TAP. Ordinary taps remain unchanged.

Because this input mapping intentionally changes the path produced by the old
replay, the controlled numbers above were captured before applying it. Final
gesture replay results are functional evidence, not another identical workload.

## Validation and artifacts

- Cache-only native regression: 644 exact baseline/current pixel and damage
  comparisons, including entrance, drill-in, clips, translated stripes and
  reopening, using reference commit `7b5c1b3`.
- Final regression: 452 exact pixel/damage comparisons, including equivalence
  at a nonzero snapped angle, plus angular/radial movement, wraparound,
  deadzone, release and tap behavior checks. Run `tools/test_launcher.ps1`;
  it is also included by `tools/test.ps1`.
- Broader native suites and 21 Python tests passed before the isolated input
  change; the final targeted tests passed afterward.
- `build/sim-menu-optimized.exe --overlay-gesture-checks` passes OS routing
  checks for taps, out-and-back drags, full circles and a subsequent tap.
- ESP-IDF and simulator builds pass. Local final image and source:
  `build/display-optimization/menu-final-image/`.
- Final application-only flash on COM3 was hash-verified. Final replay with
  angular input produced 38.65 dial and 53.00 list presentations/s, with zero
  pending/cancelled transfers, unmatched completions or submission overlaps.
  These paths differ from the controlled cache-only replay as noted above.
  Evidence: [final gestures](display-data/menu-final-gesture-check.jsonl).
- Final navigation check passed 26 apps, 13 faces, four overlays and injected
  touch; [navigation log](display-data/menu-final-navigation.jsonl). Original
  face, brightness and normal sleep policy were retained. Binary/source
  hashes are in [image manifest](display-data/menu-cache-images.json).

Physical acceptance of the optimized animation and touch feel is still needed.
The previously reported left/right fade defect has not been separately fixed.
The simulator pixel graph measures dirty area, not repeated CPU drawing work,
and its 75k reference line does not establish tear-free operation.

The SD card can wait. These small caches belong in RAM; an SD card may later
store assets or logs but does not solve the measured raster bottleneck.
