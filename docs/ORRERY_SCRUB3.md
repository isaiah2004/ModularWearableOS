# Ring scrub, round 3

Uncommitted changes against `a684695` on `watch-test`. No flashing, serial,
commits, pushes, or rebases. Windows test children inherit error-box suppression;
the renderer harness uses printed failures and exit codes instead of CRT asserts.

The 33 ms target is **not met by the calibrated model**. Expected board work is
about **37 ms average**, with modeled peaks around **41 ms**. These are predictions,
not new board measurements. The previous model's optimistic CPU-only result is
not being presented as a successful board result.

| Four 1200 ms chords, forward/back twice | Mean work | Median | Maximum |
|---|---:|---:|---:|
| Supplied a684695 board sample | 47.4 ms | 48.2 ms | 52.3 ms |
| a684695 with shared round-3 calibration | 47.40 ms | 49.89 ms | 52.82 ms |
| Current implementation, same calibration | 37.06 ms | 37.45 ms | 41.37 ms |

The replay uses the overview, date 2026-10-11, and the final ring dock. The board
window has different label counts from this scene: overview has no body labels,
and its engine recorder has three commands, not the approximately 200 retained
face commands mentioned in the request. View/dial identifiers are now in the
profiler. The supplied mixed ORRP window cannot uniquely identify the residual
cost or establish that the board scene matches this overview.

## Missing work and calibration

`NOSF work` in `firmware/main/nos_main.c` surrounds input, tick, raster, pipeline
waits, and `frame_drain()`. The older scrub checker tested `est_raster_ms`, which
is accumulated CPU cost. It also used simulator `drag`, an eased gesture with two
touch samples per frame. Firmware `inj_poll()` instead produces one linear,
integer-truncated sample per loop.

`board_swipe` reproduces that firmware sampling without device access. The new
script uses the supplied endpoints, 1200 ms, and 48 ms cadence, four times. Each
section contains 25 down samples plus a release frame. Existing eased scripts
remain separate regressions.

The original CPU accounting omitted software-double work in ephemerides and
trail layout. Target assembly confirms `ep_ecc_anomaly()` called `elems_at()`,
including six double sine/cosine evaluations it did not need. A moving frame
could compute both endpoint anomalies for eight trails, in addition to the same
Kepler solves already performed for planetary positions. The full face sky also
calculated observer longitude, latitude, elongation and phase, none of which the
face reads. Small-body calculations repeatedly rebuilt constant orbital bases.

The model now counts weighted expensive double kernels during open-ring ORRERY
ticks. The **8.909 us/unit** price fits the aggregate missing work; it includes
unseparated soft-double arithmetic and memory overhead and is **not** an isolated
libm benchmark. The same hooks/prices are applied to the archived baseline and
current sources. The modeled math contribution falls from 16.67 to 7.50 ms/frame.
The 24.3 ms supplied raster time, including waits, calibrates the open-ring
transfer floor: 23.35 ms bus plus 0.95 ms pipeline/drain. Other scenes keep their
previous model, since this residual has not been measured there.

This is a one-workload fit, not independent validation of the attribution. In
particular, the ORRP categories were averaged over a mixed window; their sums
cannot be equated with a scrub-only raster time. The remaining board uncertainty
is retained explicitly rather than claiming the unexplained cost vanished.

## Implementation

- **Exact astronomy reuse.** Face sky computes only heliocentric vectors and
  distances. Planet position evaluation publishes its eccentric anomaly for the
  trails; previous endpoint anomalies are retained. Constant conic bases,
  semiaxes, and mean motion are prepared once and reused. The existing exact
  evaluator remains an independent reference. No lower-precision planet preview
  or reduced body-update budget was introduced.
- **Event scans yield to scrubs.** An already-running scan previously ignored the
  guard on starting a new scan: it could evaluate four daily skies and 252 pair
  angles per tick. It now pauses during touch, scrub, coast, and sky settling,
  retaining existing snap events, then resumes. The isolated overview baseline
  had finished its scan before the gesture, so the reported gain does not credit
  eliminating an active scan in that case.
- **Incremental dot rows.** Sorted catalog-index row chains replace the full
  counting sort and copied dot stream. Only refreshed bodies and the small-body
  suffix are unlinked/reinserted. Stable row/catalog order preserves overlaps.
  `db` counts full builds; new `du` counts incremental updates. The exact replay
  holds `db=2` throughout the drags instead of rebuilding every frame.
- **Orbit ink cache.** The first layer precomposes its sparse AA coverage over
  `C_BG`, retaining repeated-pixel blend order. Ordinary replay copies those
  pixels. Scale previews, dimmed layers, and allocation failures retain the
  original coverage path. This is a sparse stream, not a full-screen image scan.
- **One dial moves.** During a ring drag the other two dials retain geometry and
  selected labels; calendar-entry origins still advance for exact release
  settling. Unchanged arc text runs validate and re-pin cached glyph slots
  instead of laying out the same glyphs again. Deferred exact masks still settle.
  Existing ring-band and tick caches remain in use.
- **Plate geometry cache.** Both resting plate silhouettes and the expanded NOW
  button retain exact blend lanes from the original pill rasterizer. Replay uses
  opaque word fills and cached AA edges, with ink/dim applied at replay. Date and
  offset text remain live. Animated intermediate shapes use the original kernel.

The engine already receives compact damage records. Replacing an alleged
200-command engine comparison was therefore not the relevant change for the
reproduced overview. Body labels in other views still follow moving bodies.

## Scrub-only profiling

With the existing `orrery prof 1` setting, `ORRP window=scrub` starts at an actual
ring-drag transition. Both edges flush partial windows; 30-tick windows within a
long drag stay scrub-only. Unlock, touch slop, coast, and idle are reported as
`window=general`. No new device command is required.

`ORRMEAN` reports average astronomy/layout/capture/bucket/raster times, plus nested
`sky/small/belt/events/anomaly` timings. `ORRPEAK_MATH` reports those nested timings
for the same frame as `ORRPEAK`, while `ORRPEAK_DETAIL` now identifies view/dial.
Nested timings are explanatory subdivisions and must not be added to their
parent stages. Model/host/board source labels remain explicit.

## Validation and artifacts

Run, from the worktree:

```powershell
python tools/orrery/check_scrub3.py --baseline
python tools/orrery/check_scrub3.py
python tools/orrery/check_scrub3.py --nocost
python tools/orrery/check_scrub3.py --verify
```

Artifacts live in `build/orrery/scrub3/`: `performance.json`, `memory.json`,
`verification.log`, before/after CSVs, images, frame hashes, target compile logs,
and stack-usage files. The verifier distinguishes passing functional checks from
the explicit performance **TARGET MISS**.

Validation includes all ORRERY scripts, cost/no-cost renderer tests, all frame-hash
comparisons, byte-identical settled baseline PNGs, forced-full versus damage
replay, independent ephemeris event/vector checks, and ESP32-S3 compilation.
Additional tests cover stable incremental row ordering against a separate
counting-sort oracle, exact conic results over 81 dates per catalog body,
shared planetary anomalies over 201 dates per planet, event-scan suspension,
eight baked-orbit poses, 120 clipped/dimmed/translated plate comparisons on random
backgrounds, and retained glyph-cache invalidation.

New persistent storage is in PSRAM. The removed sorted dot copy and row-sort
scratch recover 48 KiB. The remaining increase is approximately 131 KiB at the
24,576-pixel orbit capacity used by this scene, plus about 57 KiB temporary plate
construction scratch. Orbit ink scales at four bytes per allocated coverage
entry. Internal static growth is about 108 bytes (profile, plate pointer, replay flag);
the 2,176-byte hot reference cache is unchanged. Target `scene_update` stack usage
remains 1,664 bytes; `tick` grows from 224 to 304 bytes. Allocation failure falls
back to existing raster paths.
