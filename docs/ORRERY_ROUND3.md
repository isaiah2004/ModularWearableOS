ORRERY round 3 — stripe renderer, based on 869680a

The dot and orbit changes are implemented. Board targets are **not yet verified**:
no flashing or serial access was used. The calibrated model predicts the locked
full repaint, pan, and zoom targets are reachable, but still predicts a miss for
unlocked full repaint and open-ring scrubbing. Those misses are recorded below.

Implementation:

- Belt and Kuiper dots use one shaded pixel, or four dimmer pixels for the bright
  class. Small bodies use 2x2 or five-pixel plus stamps; comet heads join this
  stream while tails remain lines. No dot has an AA primitive or stripe reference.
- A stable counting sort builds one packed, y-sorted stream of projected dots.
  It runs in tick only when projection/visibility changes (including pan). The
  internal row index selects each stripe's range, including a one-pixel halo.
  Two layer sentinels preserve belt-before-trails and small-bodies-after-tails
  composition. Repeated x-clipped draws and translated stripes are supported.
- Orbits retain 12-segment chunks. Replay rejects nonintersecting segments before
  entering the hairline rasterizer. A resting locked scene has 125 retained
  commands and 541 references; open-ring scenes can exceed the hot-reference cache.
- `orr_hot` occupies exactly 3,116 bytes on Xtensa: row offsets, stripe offsets,
  and 1,024 uint16 reference slots. The slots double as counting-sort cursors
  during rebuild. Larger reference lists fall back to PSRAM. The board profiler
  adds 120 bytes, plus a category integer. Large geometry, packed dots, sort
  scratch, text, and commands stay in PSRAM. The list is 182,416 bytes in the
  64-bit host build; board pointer sizes make its text storage slightly smaller.
- Capacity/allocation failures retain the direct-render fallback. Dot overflow
  survives command-list initialization. The firmware command handler changes by
  exactly one `else if (sscanf(...))` line; no blur files were changed.

`orrery prof 1` enables the board-only microsecond timers; `orrery prof 0` disables
and resets them. Every 30 face ticks, one printf emits one line of this form
(letters below are placeholders, not invented board measurements):

```
ORRP n=30 r=30 b=0 cmd=125 ref=541 hot=1 us dots=R/C/B orbits=R/C/B planets=R/C/B text=R/C/B controls=R/C/B trails=R/C/B other=R/C/B
```

Each tuple is **raster/capture/bucket microseconds averaged over all 30 ticks**.
`r` counts ticks with raster work, `b` counts scene rebuilds, and `hot` reports
whether references fit in internal RAM. To obtain an average per rebuild from a
window with `b < 30`, multiply its capture/bucket values by `30/b`. Idle ticks
contribute zero. All dirty rectangles/stripes of a frame accumulate before the
next tick closes it. Printing happens outside measured scopes. Text includes
control labels; trails includes comet tails and planetary streaks. Small-body
and comet-head stamps are in dots. Engine clear/rotation/transfer are outside
ORRP. Timer calls add some overhead while enabled.

Dot quantization/sorting appears in `dots` bucket time, although it runs alongside
layout: consequently the existing NOSF `o` field includes that work, whereas
NOSF `clr` covers retained-reference construction and damage recording. Do not
add ORRP and NOSF timings together. There are no category timers under
`GFX_COST_MODEL`; simulator columns are operation-count estimates instead.

Calibration and estimates (milliseconds):

The old simulator's 24.35 ms was bus-limited frame time, not raster CPU. Its
counted locked-scene CPU was 12.865 ms. The board reports 37.5–39.0 ms, so the
model now applies a **3.095x multiplier only inside Orrery raster scopes**,
leaving engine clear and other faces unchanged. This brings the unchanged
869680a renderer to 37.998 ms. The same multiplier is applied to the new path;
the missing cost is not selectively assigned to dots to exaggerate the saving.
Category shares are therefore estimates, not measured board attribution.

| Raster category, reused locked scene | Before | After |
| --- | ---: | ---: |
| Belt/Kuiper/small-body dots | 14.401 | 1.940 |
| Orbits | 16.823 | 13.116 |
| Planets + Sun | 1.063 | 1.063 |
| Labels/text | 1.711 | 1.711 |
| Controls | 1.253 | 1.253 |
| Trails | 0.222 | 0.181 |
| Other face work | 1.654 | 1.654 |
| Engine clear outside those categories | 0.869 | 0.869 |
| **Raster CPU total** | **37.998** | **21.787** |

The capture fit is 5.6 us per retained command, giving approximately 6.4 ms on
the old pan sequence against the supplied 6.0–6.2 ms measurement. Its old dot
bucket proxy is 2.675 us per dot, giving 10.697 ms against the supplied 10.2–11.3
ms. Only the instrumented baseline uses that old bucket proxy. New estimates use
180 ns per integer stamp, 150 ns per orbit-bound vertex, 250 ns per reference or
command bucket visit, and 250 ns/projected input plus 150 ns/scattered dot for
sorting. **Those new operation prices require board validation.** The raster
multiplier also applies to the integer stamp estimate, conservatively pricing
it at about 557 ns.

| Pan rebuild category | Capture before | Capture after | Bucket before | Bucket after |
| --- | ---: | ---: | ---: | ---: |
| Dots | .121 | .034 | 10.697 | 1.437 |
| Orbits | 5.738 | .660 | unpriced | .244 |
| Planets + Sun | .062 | .062 | unpriced | .017 |
| Labels/text | .011 | .011 | unpriced | .004 |
| Controls | .218 | .218 | unpriced | .076 |
| Trails | .241 | .135 | unpriced | .025 |
| Other | .033 | .033 | unpriced | .011 |

Per-category medians need not sum exactly to median totals. Remaining retained
capture plus bucketing is about **1.53 ms** in this pan sequence, excluding the
new dot stream's approximately 1.44 ms. Including dots, preparation is roughly
3 ms instead of 17 ms. Open-ring controls retain more commands and cost more.

`tools/orrery/perf_round3.txt`, forced full repaint, p50 model work estimates:

| Scenario | Before | After | After maximum |
| --- | ---: | ---: | ---: |
| Locked, reused scene | 40.498 | 24.350 | 24.350 |
| Unlocked, reused scene | 45.830 | 29.619 | 29.619 |
| Pan, rebuilding frames only | 59.480 | 30.989 | 32.004 |
| Zoom, rebuilding frames only | 41.242 | 26.915 | 30.182 |
| Open-ring scrub, rebuilding frames only | 71.166 | 51.270 | 53.187 |

These work estimates use the existing transfer-overlap model. Ephemeris and
general layout computation are still unpriced, and core-1 rotation is not added
as a serial cost. Thus they are not guarantees of board work time. The supplied
board pan timing remains the authority; it is higher than the baseline model's
pan prediction. Open-ring scrub still estimates 21 ms in controls and 10 ms in
text. A single-scene aggregate fit cannot determine whether this extrapolation
is accurate; ORRP is intended to answer that before another raster change.

Verification:

- Strict cost and no-cost simulator builds; hashes match over the 359-frame
  performance script. Existing Orrery scripts and idle/clock damage assertions pass.
- Six existing damage-vs-full scripts: 3,212 identical frames. New performance
  script including open-ring scrub: another 359 identical frames.
- Independent renderer harness: 12 orbit poses against all-segment reference;
  500 translated, packed-column, dimmed, clipped dot stripes against an independent
  integer-stencil reference, including stripe/glass boundaries and NaN/infinity.
- Face translation unit compiles with Xtensa GCC 14.2 using the ESP32-S3 dynamic
  configuration, board timers enabled. No full firmware link or BLE heap test.
- Before/after locked and unlocked screenshots visually reviewed. Dots are
  intentionally sharper; their solid coverage and stable y ordering can alter
  overlaps and cause one-pixel stepping during slow pans.

Run the retained checks with `tools/orrery/check_round3.ps1` with MinGW on PATH.
CSV category columns are `orr_0_*` (raster), `orr_1_*` (capture), `orr_2_*` (bucket).
Local evidence is under `build/orrery/round3_*`, including `round3_metrics.json`.
The new 3.2 KB internal allocation, font/AA cache behavior, profiling overhead,
and open-ring performance still need validation in the merged board build.
