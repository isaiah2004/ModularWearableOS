ORRERY time-ring scrub, round 2 — 77ac23a board calibration

The fixed replay stays below 33 ms of modeled CPU work for the supplied 1200 ms chord, the faster month-scrub stress case, and their settling frames. This is a prediction, not a board measurement. No firmware was linked/flashed, no serial was opened, and no Git history was changed. A genuine ring opening still has a large final geometry rebuild; this change does not make all opening frames meet 33 ms.

`ORRPEAK_LAYOUT` has these six slots, now named in the log itself:

| Slot | Work | Supplied board evidence | Treatment |
|---|---|---|---|
| 0 | Radius mapping, screen projection, planet lighting/trails | 10.437 / 10.520 ms | Map/project only the changed belt bodies when geometry is stable; about 0.556 ms modeled during motion. |
| 1 | Orbit coverage construction and stable sorting | 0.024 / 26.832 ms | Retain coverage during time motion; tangential ring gestures cannot accidentally fold and change shrink. Warm scrub builds zero orbit caches. Real geometry changes still require a rebuild. |
| 2 | Integer dot stamps and stable row bins | 4.577 / 4.475 ms | Retain catalog-indexed stamps in existing scratch storage; requantize only changed bodies, then rebuild the linear row bins. About 0.805 ms modeled during motion. |
| 3 | Body-label placement | 0 / 0.108 ms | Existing empty-overview-label reuse retained. |
| 4 | Ring calendar/labels/tick basis | 0.334 / 0 ms | Existing calendar-window/basis reuse retained. |
| 5 | Menu layout | 0 / 0 ms | Existing menu-key reuse retained. |

Astronomy was the main missing price. The unchanged 3999-body Kepler pass is now charged 9.7 us/body: 38.79 ms, plus the existing 0.5 ms planet and 1 ms small-body allowances, reproducing the supplied 40.3 ms stage. That division of the aggregate is an attribution, not three separately measured kernels. Projection is priced as 0.25 ms fixed plus 2.55 us/projected body, reproducing 10.447 ms. A full dot rebuild is calibrated to about 4.5 ms. Existing orbit and raster calibration is retained.

During time motion, a weighted fair queue performs at most 120 exact belt solves per tick. Faster, more eccentric bodies get refreshed more frequently than distant bodies. Catalog order and stable blend order do not change. Eight small bodies/comets are updated per tick; fixed-epoch craft are prepared once. Planets use exact ephemerides on every changed interactive date, including sub-minute changes. Belt/small-body work pauses during ring geometry animation, while planets continue updating.

Once time stops, a full exact sweep targets one date. The first four ticks do at most 64 belt bodies while glyph masks settle; subsequent ticks do at most 448. The 3999-body regression converges within 13 ticks (about 433 ms at 30 Hz), including a final date jump on the release tick, then stops rebuilding. This avoids accumulating approximation error. The weighted preview's largest measured belt position error was 5.63 px at a sustained 0.75 day/frame, versus 19.08 px with uniform round-robin scheduling. Larger date jumps can produce greater transient lag. If the optional queue cannot allocate, the bounded uniform fallback still converges exactly.

Animated controls previously fell back to annular arc scans. The growing sweep now builds three concentric Wu hairlines into the existing per-ring coverage storage and replays only clipped rows. From the label-appearance phase onward it previews the warm final bezel; the temporary inner-edge displacement can be about 6.2 px. At rest, the original exact band masks remain. Modeled opening controls peak falls from 25.82 ms to 7.40 ms; the supplied board controls peak was 20.46 ms. Zero/black ink, translated/dimmed stripes, and stream capacity are covered by tests.

Visible bands own grip touch slop during both opening and closing. At the actual grip, a tangential drag scrubs and a radial pull folds; grip taps still close. The collapsed tab cannot steal a visible closing band. The exact supplied chord is exercised in both directions, including the partially open geometry. A touch awaiting drag classification also keeps astronomy on its bounded motion budget.

Two smaller raster changes preserve pixels: faint opaque belt dots share a row lookup instead of invoking the generic stamp setup for every point, and moving arc glyphs intersect their integer sampling rows with the source rectangle before visiting pixels. The latter is checked against an independent version of the original sampler. Neither path allocates glyph masks during motion.

Measured simulator results (CPU work excludes transfer/tail; frame time includes them):

| Replay | Calibrated before median / peak frame | After mean / peak CPU work | After median / peak frame |
|---|---:|---:|---:|
| Faster month-scrub stress | 88.66 / 92.78 ms | 28.40 / 32.96 ms | 36.18 / 39.61 ms |
| Supplied chord forward, 1200 ms | 87.70 / 91.52 ms | 25.37 / 31.61 ms | 34.49 / 38.26 ms |
| Supplied chord back, 1200 ms | 86.76 / 91.98 ms | 27.20 / 32.44 ms | 34.85 / 39.09 ms |

The supplied board average/max was 86.7/93 ms. The calibrated baseline now predicts that range instead of about 52 ms. Expected steady rings-open board CPU work is roughly 29–33 ms, but the 32.96 ms modeled worst case has essentially no margin. Queue operations, partial dot bins, sequential stamps and sweep construction still need board timing validation; PSRAM contention/interrupts may move the result. Transfer/tail estimates remain above 33 ms: this is not a claim of universal 30 fps.

A deliberate opening still peaks at 59.68 ms CPU work / 69.11 ms modeled frame time, mostly its final full projection/dot/orbit build. Beginning a touch before opening finishes can include that frame. Preventing accidental folds removes this rebuild from ordinary rings-open scrubs; it does not eliminate the genuine opening cost. Cold face entry also retains a complete exact initial sky pass.

ESP32-S3 compiler sizes:

| Allocation | Change |
|---|---:|
| PSRAM state and list | +1004 B |
| Belt refresh interval and optional queue, 3999 bodies | +47988 B |
| Larger shared tick/sweep streams and scratch | +73728 B |
| Total maximum added PSRAM | +122720 B = 119.84 KiB |
| Added static internal RAM | 0 B |

The simulator tracked 6,061,855 B peak against a 6,815,744 B budget after the board-chord replay, with zero allocation failures and 753,889 B (736 KiB) headroom. These are host-tracked allocations, not measured board heap availability; target structure sizes above are from the Xtensa compiler. No full-frame buffer was added. The largest compiled local stack frame decreased from 1712 to 1664 B; this is not a call-chain stack measurement.

Validation passed:

- Model and no-model builds; all top-level ORRERY scripts and existing raster regressions.
- 2488 tangential band cases across three rings and four opening/closing phases, the exact chord in both directions, and radial grip folding.
- 72 clipped/translated/dimmed animated sweep comparisons, including black ink; visual review of matching 15-frame opening contact sheets.
- All 3999 bodies, exact planet comparisons, a one-second interactive date change, 40 partial-versus-full dot rebuild comparisons, final-date convergence, and idle reuse.
- 3000 settled arc rasters against the original implementation; 360 independent motion sampler comparisons plus stripe comparisons; zero motion glyph allocations/builds.
- Settled PNGs/final hashes equal 77ac23a for month scrub, board chord and arc-motion scenarios. Cost/no-cost full frame-hash streams match. Dirty-versus-full rendering check passes.
- Motion/page/key blur and PSRAM tests. An intentional failed assertion exits 2 through the no-dialog handler. All test processes inherit disabled Windows error boxes; harness assertions print and exit.
- ESP32-S3 compile-only check and target memory sizes. No device access.

Reproduce in this worktree:

```powershell
$env:PATH = 'C:\mingw64\bin;' + $env:PATH
./tools/orrery/check_round7.ps1
python tools/orrery/check_scrub2.py
```

The second command extracts 77ac23a read-only under `build/orrery/scrub2_check`, changes cost hooks only, builds that baseline, compares it with the current model/no-model executables, and runs the supporting checks. Subsequent runs can use `--skip-baseline`. Evidence is in `build/orrery/scrub2_check/results.json`, `memory.json`, per-scenario logs/PNGs/hashes, and `build/orrery/scrub2/regression.log`.
