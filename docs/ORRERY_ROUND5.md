# Orrery round 5

Uncommitted changes on `solar-watch-face`, based on c7f8c23. No flashing,
serial access, commits, pushes or rebases. Engine/compositor/blur sources were
not changed. The simulator change is limited to Orrery pricing and its report.

Pan and warm wake meet the model's budget. Zoom motion does too, but its release
rebuild still misses. Open-ring animation and time scrub remain over budget.

## Changes

- Dots retain stable row bins over a 4096-row coordinate domain, including
  offscreen dots that a subsequent pan reveals. Raster looks up the stripe's
  source rows using the pan offset. Pan does not write belt positions or rebuild
  the dot stream. Small hit targets/comet endpoints and planet positions still
  shift; the large orbit arrays are untouched when coverage replay is available.
- Labels translate during pan and inertia. Collision placement runs once at
  settle. The existing orbit coverage phase remains after pan settles, removing
  the expensive fractional-phase rebuild. This intentionally retains a maximum
  half-pixel orbit translation approximation until a shape rebuild.
- AOD renders its small scene directly and retains the complete awake scene:
  astronomy, layout, dots, orbit coverage, labels, commands and stripe indices.
  Astronomy refreshes during AOD at its existing approximately 72-minute
  cadence. Resetting pan/zoom/time/tilt also clears azimuth inertia and obsolete
  trails. Wake normally draws the warm scene without preparation. Preparation
  has moved to AOD entry/refresh; its cost has not disappeared.
- Zoom drags resample the cached orbit coverage and transform the sorted dot
  stream at raster. Uniform scale preserves row order. Belt projection,
  re-binning and orbit coverage construction are skipped during the drag.
  Release restores projected dots and full-resolution coverage. LOOK retains
  its previous 64-segment preview and 128-segment resting shape.
- Ring calendars cache strings, widths, tick/label angle bases and their sine/
  cosine values. Year/month/day windows rebuild independently at their civil
  boundaries or timezone changes. Capture applies an angular offset and clips
  the expanded windows. Ring labels use retained widths and explicit text
  bounds instead of measuring/hashing/recording the same string every frame.
  The actual date, planetary ephemerides and belt motion are not frozen.
- ORRP adds cumulative `db`, `lb`, `rb` counters for dot builds, label placement
  and individual ring-calendar window builds; existing `ob` remains orbit builds.

## Timing and model limits

These are estimates, not new hardware measurements. The supplied board result
remains the authority: full repaint 28.5 ms, pan 34–36 ms, wake 52–58 ms.

| Scenario | Round 5 model, sending frames | Board expectation / remaining issue |
| --- | ---: | --- |
| Locked forced repaint | 28.5 ms; counted CPU 13.91 ms | About 28–30 ms, bus bound |
| PAN drag | p50/max 30.32 ms | About 30–32 ms; layout target below 1 ms |
| Pan settle | No extra pixels in the face-view settle section | In labeled zones, one placement/capture repaint, about 30–32 ms; then idle |
| Warm AOD wake | 28.50 ms | About 29–32 ms if AOD preparation completed |
| Zoom drag/release section | p50 30.64 ms; max 40.76 ms | Drag about 31–33 ms; release still roughly 40 ms |
| Ring opening | p50 40.60 ms; max 52.27 ms | Still late; shape/shrink builds and open-ring raster |
| Open-ring time scrub | p50 62.40 ms; max 67.93 ms | Still late; absolute board estimate uncertain, budget roughly 45–68 ms pending profiling |
| AOD entry after manipulation | p50 39.04 ms; max 51.16 ms | Wake preparation moved here; entry still has late frames |
| Rest / post-wake rest | Zero pixels sent | Idle; model prints 1.5 ms bookkeeping |
| Cold face entry | max 54.46 ms | Still late; model is conservative versus the supplied approximately 48 ms |

The previous 41 ms scrub estimate omitted astronomy/general layout and used a
60 ns orbit stream price. It must not be compared directly with this round's
62 ms estimate as a measured regression. A HEAD renderer rebuilt locally with
the same round-5 prices and equivalent layout/astronomy hooks estimates pan
35.2 ms, zoom 39.3 ms and scrub 63.8 ms, versus 30.3/30.6/62.4 ms here. Ring caching
is a modest improvement, not closure. That instrumented baseline and output are
under `build/orrery/round5_baseline`.

Pricing:

- Cached orbit entry 310 ns, dot stamp 625 ns, lit pixel 1254 ns. Forced-rest
  category estimates are orbits 4.151, dots 2.139, planets 2.527, text 1.714,
  controls 0.425, other 1.876 ms. The supplied board values are respectively
  4.306/2.141/2.668/1.712/0.426/1.877 ms. Scene differences remain.
- Full-frame transfer/engine allowance is 4.15 ms over the model's 24.35 ms
  floor, matching 28.5 ms. Partial transfers scale by area; idle adds none.
- General layout explicitly charges 0.25 ms plus 0.5 us/projected belt body;
  pan shifting 0.24 ms; label placement 0.2 ms. Dot collection/scatter retains
  per-body charges, increased from round 4's optimistic estimate. Together the
  old pan path now estimates about 5 ms of layout/build work.
- Belt Kepler solving is provisionally 3 us/body; planet ephemerides 0.5 ms;
  small-body ephemerides 1 ms. Ring calendar work is 0.08 ms plus 0.12 ms per
  rebuilt window; cached ring label capture 30 us/label. These are conservative
  operation budgets, not measured stage fits. The baseline still predicts
  wake at 48 ms versus board 52–58 ms, so this is not a complete hardware model.
- Scaled orbit preview charges 120 ns/visited entry plus 100 ns/output pixel
  in addition to the normal stream price. Scaled dots add 150 ns/stamp.
  Explicit CPU charges bypass legacy multipliers to avoid double-pricing.
- All preparation is outside bus overlap. Existing conservative capture
  multipliers remain for uncached commands. Open-ring capture/raster, event
  scanning, merged-blur contention and platform overhead need board profiling.

## Verification and remaining late scripts

`tools/orrery/check_round5.ps1` builds strict cost/no-cost simulators, runs the
renderer harness and all nine original script files with assertions intact,
and compares 530 cost/no-cost frame hashes. All pass. The installed host GCC
also emits existing Win32 prototype/phone-format warnings in unrelated files.

`python tools/orrery/equivalence_round5.py` compares damage/forced-full hashes:
3,665 frames match. Its copies omit idle/nolate assertions because forced
repaint cannot be idle; the originals are tested separately above.

Harness checks: warm-wake and pan cache invariants; 24 direct-Wu orbit poses;
30 scaled/translated/clipped orbit previews; 500 independently checked packed
dot stripes with pan, zoom, translation and clipping; 100 cached ring labels;
120 arcs; 660 zoom masks; 160 lit discs. All pass. A separate simulated clock
jump during AOD also wakes at 28.5 ms and returns to idle. Settled zoom and face
screenshots were inspected. The face compiles with ESP32-S3 GCC 14.2 and board
profiling enabled; no firmware link or hardware run was performed.

| Script | Late / total | Main remaining causes |
| --- | ---: | --- |
| full_face | 192 / 931 | Cold entry, zones/LOOK, time travel, rings, relock, cards/events |
| idle | 1 / 1084 | Cold entry only |
| orrery1 | 50 / 563 | Cold entry, tilt, unlock transition, zoom release/taps, relock/cards |
| orrery2 | 15 / 222 | Cold entry, ring transition, recenter/zoom, relock |
| perf_round3 | 31 / 359 | Cold entry, unlock transition, zoom release, rings/scrub |
| perf_round4 | 35 / 479 | Same, plus AOD preparation/transition |
| perf_round5 | 35 / 530 | Same; PAN and wake have no late frames |
| regress | 1 / 212 | Cold entry only |
| zoom | 5 / 200 | Cold entry and zoom taps/releases |

The explicit pan-settle section is the unlabeled face view. The harness also
checks fractional settle without dot/orbit rebuilds; hardware measurements in
labeled inner/outer views are still needed for the one-time label placement.

## Board trace checks and risks

- PAN: `o` should fall from 4.3–5.6 ms to below 1 ms. `db` and `ob` stay fixed;
  `lb` stays fixed while dragging/coasting, then rises at most once at settle.
  `sel` still captures commands, roughly the supplied 1.5 ms; full raster/bus
  time should remain similar. There should be no orbit bucket spike at settle.
- Wake: on the first awake tick, layout/dots/orbit/capture should normally be
  zero. Builds should appear during AOD entry/refresh instead. Test after both
  a short sleep and a long sleep that crosses the astronomy cadence, and after
  manipulated views. A wake coinciding with a fresh invalidation is not covered
  by the warm-scene guarantee.
- Zoom: `ob` and `db` stay fixed through a stable-shape drag, then each rises
  once on release. Preview raster rises somewhat; release still has a build
  spike. Changing view/tilt/astronomy invalidates reuse correctly.
- Scrub: `rb` changes at calendar boundaries, not for every angular update.
  It can still change every frame during fast year travel. Inspect `u`, `o`,
  text/control capture and raster separately. Astronomy and many moving labels
  remain; a lower calendar cost does not imply an under-budget frame.
- ORRP tuples remain averages over 30 ticks. Normalize using `r`/`b`; the new
  counters are cumulative. Forced-full raster should stay close to the supplied
  category values rather than the old optimistic 0.8 ms orbit estimate.

Additional persistent PSRAM is approximately 23 KB (wide dot row/cursor tables
and ring bases/widths). Internal hot storage falls from 3,176 to 2,176 bytes.
Existing orbit streams retain their high-water allocation; the test sweep still
uses about 573.5 KB for the orbit cache and 113.7 KB for zoom masks. Allocation
failure retains the slower direct path; it has no performance guarantee.

Nearest-coverage zoom previews can thicken/soften lines and temporarily move
dots by a few pixels at large scale ratios; release restores projected dots and
full-resolution orbit coverage. Pan permanently keeps the cached subpixel phase
until another shape rebuild. Shifted labels may overlap fixed UI or leave the
glass during motion; settle re-places them. Cached ring angle rotation can differ
slightly from evaluating the original approximate trig at every angle. AOD does
more occasional preparation work. Heap margin, power impact, preview appearance
and contention with the merged blur branch require hardware validation.
