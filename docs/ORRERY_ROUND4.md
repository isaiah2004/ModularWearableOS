ORRERY round 4 board informed pixel runs

Implemented on solar-watch-face after 6e05c52. The main orbit raster hotspot is
removed. The model predicts approximately 28 ms full-repaint work, 32 ms pan,
and 31 ms AOD wake. High zoom and open-ring scrubbing still miss 33 ms under the
more conservative capture pricing. No commits, flashing, serial access, or blur
session file edits were made.

Changes:

- Orbits use a six-byte coverage record (signed x/y relative to the Sun,
  effective alpha, colour index), stably sorted by row in PSRAM. Each stripe
  reads only its row range. Blending preserves RGB565 rounding and repeated
  blends at segment joins; the loop has no floating-point setup or libm calls.
  Source colour lanes and alpha quantisation are prepared before the pixel loop.
- The key contains view/morph, zoom, camera/tilt, ring shrink, visibility and
  preview resolution. Time and pan are excluded. Orbital elements are sampled
  once at the initial epoch; planetary ephemerides still advance normally.
  Projection terms also survive time-only layout updates.
- Active zoom/look uses 64 segments per ellipse; release restores 128. Active
  pan translates cached coverage by whole pixels. A fractional pan gets one
  phase refresh after settling. This exception preserves settled Wu coverage;
  it is not a shape rebuild on every pan frame. Integer pans reuse coverage.
  The complete ellipses are retained, including offscreen pixels later revealed
  by a pan. Allocation or range failures retain the old segment fallback.
- `lit_disc` tests squared distance for its interior and retains the existing
  fast-rsqrt limb coverage. This checkout already used `gfx_fast_sqrt`, not
  libm `sqrtf`. Consequently the planet saving is deliberately modest. The
  remaining per-object double `sqrt` in Earth-distance labels now uses a refined
  fast root. Remaining atan2/log/pow calls are per input event or frame/capture.
- Dots use 30 counting bins with stable row sub-buckets, preserving the previous
  overlap order. Round 3 already used counting sort, not comparison sort.
  Integer floor conversion replaces two soft floorf calls per stamp. Interior
  stamps write their one/four/five pixels without per-pixel clipping branches.
- Arc coefficients, caps and bounds are prepared in capture, then replayed with
  the original raster equations. Zoom's nine quantised segment lengths use
  lazily generated coverage masks with tight row spans. Both off/on blend passes
  are preserved. Replay no longer calls prog_poly for the zoom bar.
- When AOD resets a manipulated view home, it prepares the home orbit cache
  once during AOD entry. Wake reuses that shape. Quiet AOD/rest ticks do no work.

Costs in milliseconds:

The before column below is the supplied board ORRP window with n=r=30, b=0.
After is the re-priced simulator's equivalent locked forced-repaint estimate,
not a board measurement.

| Raster category | Board before | Estimated after |
| --- | ---: | ---: |
| Dots | 2.217 | 1.884 |
| Orbits | 20.807 | 0.833 |
| Planets | 3.202 | 2.341 |
| Text | 1.605 | 1.603 |
| Controls, locked | 0.419 | 0.419 |
| Trails | 0.205 | 0.205 |
| Other face work | 1.777 | 1.774 |
| Sum of face categories | 30.232 | 9.059 |
| Estimated engine clear | — | 0.869 |

The new orbit estimate is below 1 ms, but does not reach the requested 0.2–0.5 ms.
The full ellipses contain 13,272 coverage entries in the locked sample. Typical
unlocked controls estimate 1.97 ms; open-ring controls still reach about 7 ms,
and their text about 9.3 ms. The supplied pan controls measurement of 4.860 ms
is averaged over its own 30-tick window, so it is not a matched forced-frame
comparison to these figures.

The supplied dots bucket 2.214 ms with b=16 corresponds to 4.151 ms per rebuild
under ORRP's 30-tick denominator. The new dot build is provisionally priced at
about 2.15 ms for a complete stream; partial/offscreen streams cost less. Orbit
shape construction estimates about 2.7 ms at 1x and about 3.9 ms on the zoom
sequence, with larger full-resolution shapes exceeding 4 ms. Pan/time-only
rebuilds retain only about 0.009 ms of orbit reference bucketing, except for the
single subpixel-phase refresh after a fractional pan settles.

| Scenario | Estimated work p50 | Maximum | Target assessment |
| --- | ---: | ---: | --- |
| Locked forced repaint | 28.000 | 28.000 | CPU model 9.928 ms; board CPU target unverified |
| Unlocked forced repaint | 28.000 | 28.000 | CPU model 11.332 ms |
| Pan, rebuilding frames | 31.686 | 31.735 | Model meets 33 ms |
| Zoom, rebuilding frames | 35.450 | 35.943 | Misses 33 ms |
| Ring opening, rebuilding frames | 36.074 | 44.750 | Misses 33 ms |
| Open-ring scrub, rebuilding frames | 41.216 | 46.707 | Misses 33 ms |
| AOD wake after manipulated view | 31.022 | 31.022 | Model meets 33 ms |
| Rest / post-wake idle | no pixels sent | no pixels sent | Pass |

Pricing and limits:

- Replaced the old blanket raster multiplier with category fits. Legacy raster
  factors are dots 1 (new explicit stamp price), orbits 4.91, planets 9.32,
  text 2.90, controls 1.035, trails 3.50 and other 3.32.
- New costs bypass those legacy multipliers: coverage fetch/blend 60 ns/entry,
  integer dot 550 ns/stamp, lit disc 1100 ns/written pixel. These are provisional
  kernel estimates, not measurements. Dot collection/scatter and orbit build
  have explicit tick charges.
- Retained capture keeps its 5.6 us/command base, with category factors
  1.55/3.0/1.7/13.2/3.0/1.0/11.2. These use the supplied pan capture values
  normalised by 30/16 against the previous sim's per-rebuild category totals.
  Trails lacked a supplied capture value and retain their previous price.
  This is a conservative extrapolation: the board and scripted pan scenes do
  not have identical command populations, especially text and open rings.
  Capture residuals are charged before bus overlap, not hidden in raster time.
- Added 3.65 ms at full sent area to reserve the difference between the board's
  roughly 28 ms bus-bound work floor and the sim's 24.35 ms floor. Partial
  transfers scale that allowance by area; idle adds none.
- The supplied approximately 35–38 ms raster total exceeds the 30.232 ms ORRP
  sum. The model does not identify all of that residual CPU. If the residual
  persists unchanged, board CPU could be roughly 14–17 ms after these changes,
  despite the 9.9 ms counted CPU estimate. The <=15 ms CPU target is therefore
  not established. Ephemeris/general layout costs and merged blur integration
  still require board measurement; the transfer allowance does not prove CPU
  closure. No prices were reduced just to obtain zero late frames.

Verification:

- Strict cost and no-cost simulator builds; all eight Orrery scripts exit 0.
  The 479-frame performance/wake script has identical cost/no-cost hashes.
- Existing script assertions were not weakened. Zero late frames across the
  whole scripts was not achieved: full_face 126/931, idle 1/1084, orrery1
  62/563, orrery2 27/222, perf_round3 43/359, perf_round4 43/479, regress 1/212,
  zoom 20/200. The isolated idle/regress miss is cold entry; their quiet sections
  pass. Full-run late counts are separate from the scripts' scoped nolate checks.
- Raster harness: 24 orbit poses against the original Wu renderer on arbitrary
  backgrounds; pan/time reuse and phase refresh; 192 translated/x-clipped orbit
  stripe comparisons; 500 packed/clipped/translated dot stripes; 120 original
  versus prepared arcs; 660 original versus cached zoom cases; 160 original
  versus optimised lit discs. All pass exactly.
- Damage versus forced-full output matches for 2,487 frames across six scripts.
  Those comparison copies omit expect commands; the original scripts above
  separately run their original assertions.
- Locked, unlocked, menu and open-ring screenshots match round 3 pixel for pixel.
- ESP32-S3 GCC 14.2 compilation of the face succeeds with board profiling enabled,
  using the project build's format-truncation suppression. Its undefined symbols
  contain no sqrt/sqrtf/sinf/cosf/hypotf. No full firmware link or heap soak was run.

Run `tools/orrery/check_round4.ps1` with MinGW gcc/objcopy on PATH. Evidence lives
under `build/orrery/round4_test`, `round4_equivalence`, `round4_images_before`,
`round4_images_after` and `round4_force_final`. The runner handles native toast
warnings without treating stderr as a failed exit on Windows PowerShell 5.

What to inspect with `orrery prof 1`:

- At forced rest: b=0, orbits raster around 800–1000 us rather than 20807 us;
  dots around 1900–2200 us; planets around 2300 us. Orbit capture/bucket should
  remain zero. Investigate higher orbit raster first: the 60 ns stream price
  depends on sequential PSRAM access and the inlined blend loop.
- `ob` is the cumulative successful orbit cache build count; `op` is the current
  entry count. `ob` must stay unchanged during time scrub and active pan, rise
  for changed zoom/tilt/view/shrink, and may rise once at fractional pan settle.
  Release from a 64-segment preview also rebuilds once at 128 segments.
- Cache construction is in the orbits bucket column. Normalise by actual shape
  builds, not scene rebuilds. Dots bucket is still collection plus sort, not just
  the scatter. All tuple values remain averages over 30 ticks; r and b disclose
  raster and scene rebuild counts. ob is cumulative, so compare adjacent lines.
- Check unlocked zoom/menu versus open time rings separately. Remaining misses
  are concentrated in text/control capture and high-zoom cache creation. Collect
  those categories before treating the conservative capture fit as measured cost.
- After wake settles and during ordinary rest, confirm zero sent pixels. The
  simulator still prints a bookkeeping-only 1.5 ms estimate on idle frames.

Risks:

Additional storage is PSRAM: two orbit streams, a 32 KB row/cursor index, 5 KB
arc coefficients, and 113,732 bytes of zoom masks. After the high-zoom sweep,
the retained orbit capacity was 45,056 entries, or 573,496 bytes including sort
scratch/index on the host. These allocations persist at their high-water mark;
larger 4x poses can grow further. Internal hot storage grows by 60 bytes to
3,176 bytes, plus small state/pointer fields. Allocation failure preserves the
slower direct rendering path. PSRAM contention with the merged blur renderer
and the largest-view heap margin need hardware validation.

During motion, integer pan translation and 64-segment previews can differ from
the old subpixel animation; settled coverage is restored. Fixed initial-epoch
ellipses intentionally stop secular orbital-element updates during long time
travel. Very distant dates can therefore place planets slightly off those fixed
ellipses. The private prepared arc equations and zoom mask alpha decoding must
stay in sync with core gfx/prog blend rules; the equivalence tests protect them.
