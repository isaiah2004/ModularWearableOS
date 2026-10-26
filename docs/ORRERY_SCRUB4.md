# Time-ring scrub, round 4

Changes against `0555b02`, branch `watch-test`. No commits, pushes, rebases,
flashing, or serial access. The pre-existing untracked `scrub_exact.txt` is unchanged.

Expected board work is **about 33 ms average**, versus the supplied **39.9 ms**.
The shared calibrated replay predicts **32.89 ms mean / 32.75 ms median /
40.54 ms maximum**. The mean target is met in the model; the **every-frame 33 ms
target is not met**. These are predictions, not new board measurements.

## Internal RAM

The ESP32-S3 writable-symbol audit reclaims **11,196 bytes net**. Applied to the
supplied `internal_free = 16,099`, that projects **27,295 bytes (26.66 KiB)**,
above the 24 KiB target. Runtime free memory has not been measured after this change.

| Storage | Previous internal bytes | Change |
|---|---:|---|
| Arc keys / hash buckets / pins | 6,144 / 512 / 384 | One 7,040-byte PSRAM allocation through `nos_mem_alloc`, included in the arc budget |
| Duplicate stripe references / offsets | 2,176 | Removed; read the existing PSRAM list directly |
| Orbit construction scratch | 1,536 | Moved into the budgeted PSRAM face state |
| Profiler | 452 | Moved into the PSRAM face state; expanded to 500 bytes for mean substage counters |
| New index pointer / engine compare timer | — | 4 + 4 bytes internal |

The face now has **64 bytes** and `ui_arc.c` **48 bytes** of writable static
storage on the target. These are pointers, scalar flags, timestamps, and counters;
there are no retained internal arrays in those modules. The new four-byte engine
timer is a per-frame scalar. The plate pointer and raster replay flag introduced
in round 3 remain internal (four bytes each). No new per-pixel cache stays internal.

The actual round-2-to-round-3 writable-static increase in these modules was
**108 bytes**, not 8.7 KB. The round-3 state/list/orbit/plate allocations already
use the firmware's `NOS_PSRAM_TRACKING` path; the underlying allocator requests
`MALLOC_CAP_SPIRAM` exclusively. Thus the supplied 8.7 KB runtime loss is **not
explained by the source/object static delta**. A linked firmware/heap comparison
would be needed to identify that extra runtime/build difference. This report
does not claim that moving 108 bytes explains the board observation.

New PSRAM payload versus round 3 is approximately **38,456 bytes**: 2,064 bytes
of face state, the 7,040-byte arc index, and six optional 4,892-byte command
caches. The seven new allocations add 56 budgeted header bytes. Existing large
round-3 arrays remain in PSRAM. No budget was increased.

## Calibration and remaining costs

The archived 0555b02 replay is calibrated to **39.899 ms**, before comparing the
changed renderer. It retains the original operation prices and adds the same
**2.84 ms** correction to both versions: 0.90 ms for the measured raster change
from 24.3 to 25.2 ms, plus **1.94 ms left unassigned** to input, engine, memory,
and other overhead. That residual is not credited as an optimization saving.

The four linear 1200 ms chords retain the previous harness's 48 ms sampling for
the controlled A/B (104 painted samples including release). Down samples alone
predict **32.66 ms mean / 36.82 ms maximum**, versus 39.89 / 44.21 before.
Actual cadence and the board's 28-frame measurement window differ; this is a
single-workload fit, not independent validation. New float-kernel prices are
provisional (1.5 us for sin/cos, 3 us for hyperbolic operations). PSRAM metadata
latency and contention still need board validation; the forecast has no margin
for claiming a guaranteed 33 ms deadline.

The scrub-only profiler windows contain 80 ticks, excluding touch slop, release,
coast and idle. Their modeled means are below; nested rows must not be added to
their parent again.

| Cost per scrub tick | Before | After |
|---|---:|---:|
| Astronomy total | 9.898 ms | 3.120 ms |
| Planets / Pluto, within astronomy | 5.177 | 0.755 |
| Small bodies, within astronomy | 3.275 | 0.919 |
| Belt solves / scheduling, within astronomy | 1.446 | 1.446 |
| Events / extra anomaly solves | 0 / 0 | 0 / 0 |
| Layout total | 1.056 | 1.013 |
| Projection, within layout | — | 0.552 |
| Dot rows, within layout | — | 0.380 |
| Labels / orbit rebuild / menu, within layout | — | 0 / 0 / 0 |
| Ring calendar, within layout | — | 0.080 |
| Capture | 1.989 | 1.396 |
| `ui_arc` prepare / retain, within capture | — | 0.053 / 0.015 |
| Command buckets | 0.187 | 0.187 |
| Face raster CPU, overlaps transfer | 16.813 | 16.812 |

Engine record/compare now have separate real timers in `ORRMEAN_ENGINE`, labeled
`source=host` or `source=board`. The host run is about 6 us / below 1 us; those
numbers are **not board prices**. Their unknown board contribution remains in
the 1.94 ms residual. The engine still compares only a few aggregate damage
records. The supplied 25.2 ms raster/transfer stage is the board floor used here,
not the 16.8 ms face-only CPU counter.

`ORRMEAN_LAYOUT` now names projection, orbit work, dots, labels, calendar, menu,
and arc preparation/reuse. Existing astronomy detail reports sky, small bodies,
belt, events, and anomaly work. Profiler arrays themselves live in PSRAM.

## Changes

- Open-ring motion uses native float orbit kernels. Julian dates, secular
  elements and multi-turn phase reduction retain double precision. Ill-conditioned
  or unconverged conic cases fall back to the exact evaluator. Events/cards and
  settled rendering keep the existing double APIs.
- Release forces exact refresh at the original retained refresh epoch, even if
  the date did not change. Using the newer RTC timestamp caused a few different
  settled pixels and was corrected. Early settling updates two small bodies per
  tick, then eight; motion still updates eight. The exact sweep completes within
  the existing 13-tick belt settling envelope.
- Unchanged-row dots update x/ink in place, retaining catalog order without an
  unnecessary unlink/reinsert.
- Stationary dial commands, the grip, and static DATE/NOW plate commands are
  retained in optional PSRAM caches. Dynamic date/offset text keeps its original
  draw order. Glyph slots validate and re-pin; direct previews rebuild at rest;
  animated ring geometry invalidates backing caches. Allocation failure uses the
  original construction path.
- Arc index allocation failure, mask allocation failure, retry and drop all
  retain drawable fallback and correct budget accounting.

## Verification

Run from the worktree:

```powershell
python tools/orrery/check_scrub4.py --baseline
python tools/orrery/check_scrub4.py
python tools/orrery/check_scrub4.py --nocost
python tools/orrery/check_scrub4.py --verify
python tools/orrery/check_scrub4.py --support
python tools/orrery/audit_scrub4_memory.py
```

Artifacts are in `build/orrery/scrub4`: `performance.json`, `internal_memory.json`,
`memory.json`, CSVs, PNGs, hashes, target objects/stack reports, and test logs.
The verifier separately reports `mean_target_met` and the strict peak target.

Checks cover all 19 ORRERY gesture scripts; byte-identical settled PNGs/final
hashes against 0555b02; all cost/no-cost frame hashes; forced-full versus damaged
rendering; renderer regression suites in both modes; independent exact ephemeris
vectors/events; PSRAM stress and fault injection; blur kernel, gate, page,
notification and settings suites; 3,000 arc reference rasters and 360 motion
stripe previews; and ESP32-S3 compilation/symbol sizes.

New checks cover 201 dates per body in four views at 4x zoom/tilt (worst tested
preview error **0.000977 pixels**), exact unchanged-date release, arc index/mask
budget failures, and 28 warm-versus-cold command captures across reopen geometry,
three dials, NOW press states and animated plates. Test assertions print failures
and exit; the blur harness explicitly tests that path without a Windows dialog.

The round-2 and round-3 checkers retain their numerical checks, with probes updated
for the removed internal cache and round 3's shared +2.84 ms calibration. Their
settled baseline comparisons remain enabled. CPU-only legacy limits are distinct
from the round-4 work-time deadline; passing them does not mean every board frame
will finish within 33 ms.
