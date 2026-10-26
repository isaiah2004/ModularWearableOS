# Orrery round 7

Local changes on `solar-watch-face`, based on `f5113b4`. No commit, push,
rebase, flashing, serial access, firmware link or new board measurement.

## Root cause and trace correction

`orr_replay()` explicitly discarded the caller's dim (`part.dim=0`), replayed
bright retained content, then scanned every pixel of the stripe and called
`gfx_ink` on every lit pixel. Round 6 skipped black conversions but retained
the scan and per-lit-pixel work. Its full-stripe iris replay kept that work
nearly constant as the iris grew. This is the confirmed code path behind the
expensive dimmed under-layer; the supplied timings do not independently isolate
scan, ink conversion and cache/contention costs.

All primary writers already support ink dim: cached orbit runs (including
scaled previews), dot stamps, shaded planets, text, prepared arcs and zoom
coverage. Replay now preserves `fb->dim` and removes the post-composition loop.
The retained orbit-segment allocation/capacity fallback also now converts its
ink before calling the raw hairline kernel. There is no dim-dependent cache
key or rebuild. Dot conversion is charged once per stamp in the model.

The **12.1 ms `sel` sample is not a clean selector measurement**. Hardware-only
`ORR_TIME_END(2, tcommands)` added face command-capture time to `nos_dbg_us[2]`,
the same counter used by iris selection. Astronomy, layout and bucketing also
polluted `u`, `o` and `clr`, respectively, even with profiling disabled. Those
writes are removed; tick timings now belong exclusively to ORRPEAK. A no-cost
build regression forces a recapture and verifies all four compositor counters
remain untouched. The original sample cannot be split into capture and actual
selection afterward; claiming that the selector itself cost 12.1 ms would be
unsupported.

## Exact nos/nos.c change

Only the iris explanatory comments and the under draw inside the existing
`s_scratch` branch changed. When the inner circle has a common chord across
the stripe (`I1 > I0`), under draws into the disjoint clips `[c0,I0)` and
`[I1,c1)`. If there is no common chord, it draws `[c0,c1)` once. Empty clips
return immediately through the existing subview check.

Pixels rasterise into the existing internal destination stripe. The existing
edge-span saves to scratch, over draw, selection, blur, rim, seed, radius,
transition timing, no-scratch fallback and cost hooks are unchanged. There is
no new engine API, buffer or allocation. Both span calls use their x clips;
orbit runs seek them and primitive bounds reject absent content. They do not
rerasterise full stripes. Fully covered stripes skip under.

The clips are conservative across at most 16 rows: a small amount of hidden
content adjacent to the curved edge can still rasterise. This avoids dispatching
the entire scene once per row and preserves the blur band. It is not an exact
per-pixel circle mask.

## Calibration and predicted timing

The reproducible baseline compiles the actual round-6 face and engine from
`f5113b4` against the same current cost model. Only observational profiling and
the old dim loop's cost hook are changed in that baseline; pixels are not.
`orr_postdim` charges 110 ns per scanned pixel plus the old 120 ns per lit pixel.
This is an **aggregate board fit**, including scan/call/cache effects, not a
claim that the isolated dim arithmetic costs 110 ns. It reproduces approximately
40.9 ms face CPU plus 0.7?1.9 ms copying during Settings entry, matching the
reported flat 40?44 ms under-layer. The new renderer emits no `orr_postdim` work.

Sending-frame estimates, from `tools/orrery/equivalence_round7.py`:

| Scenario | Repriced round 6 p50 / max | Round 7 p50 / max | Round 7 late |
| --- | ---: | ---: | ---: |
| Settings first open | 52.00 / 52.93 ms | 28.50 / 28.50 ms | 0/12 |
| Settings second open | 52.00 / 52.93 ms | 28.50 / 28.50 ms | 0/12 |
| Notifications open, off-centre | 51.17 / 54.40 ms | 28.50 / 28.50 ms | 0/14 |
| Notifications close | 50.42 / 55.39 ms | 28.50 / 28.50 ms | 0/14 |
| Pan drag | 29.57 / 29.57 ms | 29.57 / 29.57 ms | 0/15 |
| Pan fling and settle | 29.49 / 29.69 ms | 29.49 / 29.69 ms | 0/41 |
| Warm wake, each of two cycles | 28.50 / 28.50 ms | 28.50 / 28.50 ms | 0/1 |

The pre-fix Settings iris frames range from 49.39 to 52.93 ms; the final
transition-complete frame is cheaper. The new face raster through those iris
frames is 13.74, 13.22, 12.55, 11.18, 10.10, 8.86, 7.03, 5.28, 4.21, 3.18,
2.40 ms. Including the model's entire save/restore copy charge conservatively
gives approximately **14.4 ms initially, shrinking to 3.7 ms**. Actual `u`
excludes restore work, which is in `sel`.

Expected board hypothesis: **u about 14?15 ms at entry, shrinking toward
3?4 ms; total work about 28.5?31 ms**, assuming the supplied ordinary raster
and transfer costs carry over. Zero late openings is the target and the model
result, not a measured board guarantee. The model's flat 28.5 ms totals are its
calibrated transfer floor, not evidence that every real frame takes exactly
28.5 ms.

## Pan, wake and ORRPEAK

No saved board ORRPEAK samples were supplied or found in the worktree. The
reported 37.6 ms pan and 44.7 ms wake maxima therefore remain **unattributed
and unverified**. No speculative pan/wake algorithm change was made.

ORRPEAK now works in the simulator using operation counts, never desktop wall
time. `set orrprof 1` enables it. Board `orrery prof 1` remains available.
`ORRPEAK_DETAIL` adds the same peak frame's context
`AOD/transition/drag/coasting` and raster categories
`dots/orbits/planets/text/controls/trails/other`; it labels model, host or board
output explicitly. Zero-cost windows no longer repeat a stale previous peak.
The extra board profiling state is about 92 bytes; there are no new caches.

Representative final **model** ORRPEAK samples from the asserted script:

| Frame / case | Astronomy/layout/capture/bucket/raster, us | Dot/orbit/label/UI builds |
| --- | --- | --- |
| 416 / pan | 0/240/729/108/14416 | 0/0/0/0 |
| 483 / fling | 0/0/729/107/14371 | 0/0/0/0 |
| 523 / final settle | 0/440/650/98/9965 | 0/0/1/0 |
| 565 / warm wake | 0/0/0/0/13515 | 0/0/0/0 |

The pan peak raster is chiefly orbits 4.18 ms, planets 2.59 ms and controls
2.46 ms; wake is orbits 4.71 ms, planets 2.53 ms and dots 2.14 ms. Neither
model peak rebuilds geometry. The model still misses the board tails; its
zero-late pan/wake result must not be used to declare them fixed. ORRPEAK ranks
face CPU, not whole-frame work, and excludes other-screen, engine/transfer and
logging time outside its measured scopes.

## Verification and risks

- Strict cost and no-cost simulator builds and all eleven original script
  assertion sets pass. The raster harness passes with both operation counters
  and the hardware timer code path compiled.
- 8,442 frames match byte-for-byte against the **ink-dim-only face with the
  old engine**, isolating the engine clipping change. The same frames match
  damage rendering versus forced-full rendering. The sweep includes all nine
  faces, blur off/on, three origins, opening, closing and reversal.
- 684 cost/no-cost frame hashes match; final profiler-only adjustments preserve
  those hashes. Tests cover translated/packed stripes, arbitrary x partitions,
  direct-versus-retained dimmed orbit fallback, all 33 ink levels on a nonblack
  background, dot stamps, text, planets, arcs, zoom, and wake/pan cache invariants.
- ESP32-S3 GCC 14.2 compiles the face and engine. Existing snprintf truncation
  warnings remain. The target compiler wrapper failed to locate its path; the
  toolchain's actual compiler with its ESP32-S3 dynconfig compiled successfully.
- Visual inspection of the mid-iris baseline/fixed PNGs passed. Ink dim before
  AA intentionally changes RGB565 rounding: in the inspected frame 3,679 pixels
  differ by at most 1 red / 2 green / 1 blue code steps. Undimmed rendering uses
  the existing kernels. The 8,442-frame claim is engine-only equivalence, not
  pixel identity to round 6's post-dim rendering.
- Conservative curved-edge overdraw, PSRAM/DMA contention, allocation fallback,
  profiler overhead and the separately merged blur engine remain board risks.

Run with MinGW gcc/objcopy on PATH:

```powershell
./tools/orrery/check_round7.ps1
python tools/orrery/equivalence_round7.py
```

Evidence: `build/round7_test.log`, `build/round7_equivalence.log`,
`build/orrery/round7_test/`, `build/orrery/round7/` (baseline, frame CSVs,
ORRPEAK logs, pixel images and target compile log).
