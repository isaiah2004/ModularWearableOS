# Final ring read-out

Design 5 on the white capsule from design 4 is now the only read-out.
`ringstyle 5` is accepted as a compatibility no-op; other values are rejected.
The original five-design comparison is preserved at commit `4e25f20`.

Build both final executables, then run the review:

```powershell
./sim/nos_build.ps1 -Strict -Out nos_sim_ringfinal.exe
./sim/nos_build.ps1 -Strict -NoCostModel -Out nos_sim_ringfinal_nocost.exe
./tools/orrery/ringread/render.ps1
python tools/orrery/ringread/memory.py
```

`review.py` reconstructs and builds the baseline from Git commit `4e25f20`
inside `build/orrery/ringfinal/baseline-source/`; it never changes the checkout.
Python/Pillow and MinGW GCC are required. The optional memory probe uses the
installed ESP32-S3 compiler to compile objects and read symbol sizes, without
linking firmware or communicating with a device. All child processes inherit
Windows error-dialog suppression, including the historical baseline.

`style5.txt` retains the same date and touch sequence as the original review,
including a month-ring drag, +99 D, NOW return and 3D tilt.
`switch_check.txt` checks the compatibility command and a negative-date return.
The final renderer uses actual bilinear-sampled tangent glyphs.

## Final implementation

The whole split dock is a C_PRI white AA capsule, with dark DATE/TIME labels,
dot numerals, divider and state. NOW uses a dark capsule with white text. The
live dot remains the sole red UI accent. Travel, NOW return, ring hit testing
and 3D tilt retain their input behavior. The menu handle fades while the rings
open because it previously struck through the right-hand year labels; the
year dial already has touch priority in that region. Locked and AOD rendering
remain pixel-identical to the baseline captures.

All year/month/day labels, including selected values, use the new API in
`nos/ui.h` and `nos/ui_arc.c`:

```c
ui_text_arc_begin(); /* invalidate prior runs, release their cache pins */
ui_text_arc_prepare(&run, UI_F_SMALL, cx, cy, radius, angle,
                    "2026", UI_CENTER, 1); /* tick/layout only */
ui_text_arc(fb, &run, ink);                 /* stripe replay */
```

Angles are clockwise from noon; UI_LEFT/CENTER/RIGHT mean start/centre/end in
reading order. Glyph advances follow the circular baseline. A whole run flips
at the side when its anchor enters the bottom half (90 < angle < 270): reading
direction reverses and glyph tops face inward. Upper glyph tops face outward.
This keeps adjacent characters on one consistent side of the flip. Masks are
keyed by font, glyph and 2-degree rotation, with bilinear alpha sampling.
The face offsets the baseline by 5 px on either side of the ring centreline.

`prepare` returns success/failure. Runs hold at most 16 byte glyph codes;
rotated glyph bounds must fit 32x32. Invalid input or cache exhaustion leaves
an empty run. One retained owner must replace all its runs after `begin`.
No allocation, libm, mask lookup or resampling occurs in `ui_text_arc` draw.
Each run records one damage command, not one per glyph. Orrery groups those
behind its scene recorder (at most 10 commands in the approval sequence,
well under the 320-command engine cap).

The 384-slot glyph cache evicts least-recently-used unpinned masks. Live runs
pin their slots until the next `begin`; `ui_text_arc_drop()` frees the whole
allocation only when no slots remain pinned. `ui_text_arc_stats()` reports
allocated bytes and cumulative mask builds/hits. The private budget helper
is entirely in `nos/ui_arc.c`, capped at 448 KiB. There are no new
`nos/psram.c` or `nos/psram.h` files or references.

For the watch-test merge, the glyph allocation seam is the single
`hal_big_alloc(bytes)` call in **`ui_arc_alloc()`**. The matching release is
`ui_arc_free()`. The optional fixed bezel-edge cache has its own single
`orr_band_alloc()` call, bounded below 64 KiB, following this face's existing
allocation convention; route it through the real owner budget too. It bakes
32 rows per tick after unlocking and falls back to prepared arcs if unavailable.
Ring ticks are grouped into three commands with stripe indices. These caches
offset the cost of the new curved text without changing their raster output.

The simulator now declares `CreateWaitableTimerExW` through the Windows Vista
API level, fixing the implicit-int/HANDLE initialization warning.

## Costs and memory

Same input, 466 px, 16-row stripes, existing re-priced ESP32-S3 model,
33.3 ms budget. Times include tick preparation. Exact results: `costs.json`.

| Section | Late before -> after | Median ms before -> after | Max ms before -> after |
| --- | --- | --- | --- |
| Opening | 10/21 -> 10/21 | 5.560 -> 4.968 | 45.682 -> 45.409 |
| Rings open, resting | 0/30 -> 0/30 | 1.500 -> 1.500 | 1.500 -> 1.500 |
| Scrub/drag | 68/71 -> 67/71 | 55.952 -> 54.098 | 56.353 -> 56.389 |
| Return NOW | 13/39 -> 12/39 | 1.500 -> 1.500 | 56.806 -> 55.933 |
| Tilt | 12/95 -> 12/95 | 1.500 -> 1.500 | 45.504 -> 41.836 |

The approval sequence drops from 106 to 103 late frames out of 396. Scrub's
single maximum increases 0.036 ms while its median, p95 and late count improve;
this is not a claim of meeting 30 FPS during scrubbing. Mask-generation prices
are explicit conservative estimates, not new board measurements: 350 ns per
bilinear sample, 40 ns per output bounding-box pixel, 12 us per miss plus
12 us per glyph layout; replay adds 70 ns per coverage visit to mask blending.

ESP32-S3 compiler-confirmed added PSRAM is **525,072 bytes (512.77 KiB)**:

| Allocation | Added bytes |
| --- | ---: |
| Rotated glyph masks | 425,472 |
| Fixed bezel coverage | 64,948 |
| Retained runs/ticks/stripe indices | 34,652 |

The main face state size is unchanged. The retained list grows from 201,888
to 236,540 bytes on the target ABI. These numbers are additions to the face's
existing allocations, not total system heap use. Simulator pointers/alignment
make its glyph allocation 427,008 bytes; eviction tests verify it drops to zero.
`memory.json` contains the target sizes. The global budget/owner integration
and real hardware frame timing remain merge/device follow-ups.

## Validation and approval files

All 13 current `tools/orrery/**/*.txt` scripts exit 0. Cost and no-cost raster
tests pass, including independent cardinal rotation oracles, 180 curved text
stripe/translation/clip cases, glyph eviction, exact cached bezel coverage,
and the retained-allocation failure guard. Cost/no-cost and damage/full-frame
hashes agree for all 396 approval frames. AOD and locked PNG pixels match the
freshly built baseline. Test failures print and return nonzero with Windows
crash/assertion dialogs disabled. Logs are `raster.log`, `review.log` and
`validation.json` under the output directory.

All screenshots are under `build/orrery/ringfinal/`:

- `sheet.png`: before row from 4e25f20, final row; NOW, +99 D, mid-drag, 3D tilt.
- `labels_before_2x.png`, `labels_after_2x.png`: nearest-neighbor 2x crops,
  including both side flip points and the selected values.
- `after/style5_now.png`, `after/style5_travelled.png`,
  `after/style5_drag.png`, `after/style5_tilt.png`, `after/style5_returned.png`.
- `after_locked/r1_aod.png`, `after_locked/r2_wake_locked.png`.

No commits, pushes, rebases, flashing or serial operations were performed.
