# Orrery round 6

Uncommitted on `solar-watch-face`, based on `05e8a2b`. No commits, pushes,
rebases, flashing or serial access. Results below are simulator estimates and
pixel checks, not new board measurements.

## Findings

The iris could call the under screen four times for one stripe: left and right
outside columns, then two edge spans in scratch. Orbit replay scanned whole
source rows on each call. Dot replay fetched whole source rows and entered the
stamp routine even when every pixel missed the x clip. Text command bounds
already rejected nonintersecting x clips.

There was a second substantial cost: retained-scene dimming called `gfx_ink`
for every pixel, including the black sky. This is more work than the engine's
packed-lane dim kernel, but the simulator priced it as that kernel. The board's
iris scratch is PSRAM (`firmware/main/nos_main.c`), so drawing/blending the edge
spans directly there also differs from the ordinary internal-stripe raster.
The supplied board timings do not isolate these contributions individually.

Dim, framebuffer translation and clipping are absent from scene, orbit and dot
cache keys. Raster does not call scene preparation. Regression tests vary dim
and translation and partition the x clip without changing the scene key or
orbit/dot/label build counters. Transition-driven rebuilding is not the cause.

For ordinary pan, the belt/sky cadence is approximately 72 minutes, not one
solve per moving frame. Dot and orbit builds stay fixed; labels shift during
motion and re-place once at settle. Fixed controls nevertheless repeated text
capture, arc preparation and command generation on every pan frame. That work
is now cached. The supplied **13/40 late, 37.3 ms maximum pan tail has not been
reproduced or conclusively attributed**. The model already missed that tail
before these changes; its new zero-late result is not proof of board closure.

## Changes

- Orbit entries are stably sorted by x within each row using two radix passes
  followed by the existing row scatter. Equal-pixel blend order is preserved.
  Replay binary-searches the clipped range, including translated/scaled views,
  and computes the destination row address once per row. Existing scratch and
  cursor arrays are reused; no new orbit allocation.
- Dot stamps reject x clips before ink conversion and per-pixel halo tests.
  Their original ordering is retained because overlapping multi-pixel stamps
  cannot safely be reordered by centre x alone.
- The retained dim pass skips black and applies the original `gfx_ink` only to
  lit pixels. Neutral-grey rounding and composed RGB565 pixels are unchanged.
- A bounded PSRAM cache retains fixed controls' commands, strings and prepared
  arcs across pan. Its key excludes pan/moving-pan state but retains all other
  scene inputs. Capacity/allocation failure takes the existing capture path.
  Host size is 4,960 bytes (slightly smaller with 32-bit pointers on the board).
- `orrery prof 1` adds cumulative UI builds/hits (`ub`, `uh`). `ORRPEAK` reports
  the worst measured face-CPU frame in each 30-tick window:
  `us=astronomy/layout/capture/bucket/raster`, plus that frame's
  `builds=dot/orbit/label/UI` deltas. These are one frame's measurements, not a
  sum of independent maxima. Engine, transfer and other-screen time are outside
  these scopes. This separates preparation and raster candidates for the pan tail.

### Exact engine change

Only the iris under-layer block in `nos/nos.c`, its cost hook and introductory
comment changed. On a crossing stripe with scratch available:

1. Draw under once into the **internal destination stripe**, unless the inner
   circle covers the entire clipped stripe.
2. Copy only the existing two edge spans to the existing scratch buffer.
3. Draw over, then use the existing copy-back and blur-band selection loops.

The no-scratch fallback, radius/chord geometry, dim factor, rim, seed, blur
weights, dissolve, transition timing and APIs retain their existing behavior.
No new engine buffer or allocation. Keeping raster in the internal stripe is
intentional: scratch is PSRAM on this board. The change is local to the iris
composition block for merging with `blur-engine`.

## Timing and calibration

`perf_round6.txt` opens Settings centrally and notifications off-centre, closes
both, then exercises pan and inertia. Sending-frame estimates:

| Scenario | Repriced round-5 maximum | Round-6 p50 / maximum | Round-6 late |
| --- | ---: | ---: | ---: |
| Settings open | 60.52 ms | 29.85 / 30.78 ms | 0/12 |
| Notifications open | 61.36 ms | 29.02 / 32.25 ms | 0/14 |
| Settings close | 60.29 ms | 28.5 / 32.50 ms | 0/12 |
| Notifications close | 60.25 ms | 28.5 / 33.24 ms | 0/14 |
| Pan drag | 30.32 ms | 29.57 / 29.57 ms | 0/15 |
| Pan fling/settle | 30.44 ms | 29.49 / 29.69 ms | 0/41 |
| Warm AOD wake (round-5 script) | 28.50 ms | 28.50 / 28.50 ms | 0/1 |

Pan rest and wake rest send zero pixels. The full round-6 script still has late
cold entry and unlock-animation frames; zero-late assertions apply to the four
iris sections and pan sections. Idle assertions remain enabled in normal runs.

Expected board work is roughly **29–33 ms for opening over the orrery** and
**29–32 ms for ordinary warm pan**. These are hypotheses for the next board
run, not guaranteed bounds. In particular, the unexplained 37.3 ms pan tail
may remain, and notification close has almost no model margin.

Pricing is explicit:

- Legacy full-pixel retained dim: 160 ns/visit, bypassing category multipliers.
  This aggregate fit reproduces an opening peak of 61.4 ms versus the supplied
  61.6 ms, compared with the old model's approximately 41 ms Settings peak.
  It is not an independent measurement of the dim kernel or PSRAM contention.
- Sparse dim: 8 ns/scan pixel plus 120 ns/lit pixel, provisional.
- New edge-span save: 60 ns/pixel for sequential PSRAM writes, provisional;
  counted inside the stripe pipeline, not as pre-flush preparation.
- Orbit sort: 80 ns per counted radix operation; row seeks 1.2 us/row;
  rejected dot stamps 100 ns. Existing accepted stream prices remain.
- Cached UI copy: 1 us/command plus 2 us/string or arc, provisional. Reused
  commands still pay normal stripe bucketing costs.

The baseline comparison compiles the actual `05e8a2b` face and engine against
the same current simulator. It does not compare differently priced executables.
Cold entry increases from approximately 54.5 to 56.6 ms in this script because
sorting and initial UI-cache capture are charged. Warm replay avoids that work.

## Verification

Run from the worktree with MinGW `gcc` and `objcopy` on PATH:

```powershell
./tools/orrery/check_round6.ps1
python tools/orrery/equivalence_round6.py
```

- Strict cost and no-cost builds; all ten orrery scripts with original
  assertions; 414 cost/no-cost frame hashes: pass.
- Harness: 48 dimmed/translated x-span partitions; 24 cached-versus-uncached
  control captures in face/inner views; labels rebuild once at settle; warm
  wake/pan cache invariants; 24 direct-Wu orbit poses; 30 scaled previews;
  500 independently checked clipped/packed dot stripes; 100 ring labels;
  120 arcs; 660 zoom masks; 160 shaded discs: pass.
- **7,803 frames match byte-for-byte** both before/after and damage/full.
  This includes all nine faces, blur off/on, three iris origins, opening,
  closing and reversal during opening. Comparison copies omit `expect`
  commands because forced repaint cannot satisfy idle assertions; originals
  run separately above.
- ESP32-S3 GCC 14.2 compiles the changed face (board profiling enabled) and
  engine. No firmware link or device run. Existing formatting and host Win32
  prototype warnings remain.
- `git diff --check`: pass.

Generated evidence is under `build/orrery/round6_test` and
`build/orrery/round6_equivalence`; runner logs are `build/round6_test.log` and
`build/round6_equivalence.log`.

## Remaining risks

Board PSRAM/cache behavior, DMA overlap and the merged blur branch are not
fully represented by operation counts. The pan tail needs the new `ORRPEAK`
breakdown alongside NOSF. A pan crossing an astronomy refresh can still build
geometry; concurrent time travel intentionally continues to solve ephemerides.
Allocation failures preserve the slower fallback. Sorting adds cold/shape-build
work, and the UI cache adds about 4.8 KiB of PSRAM. Existing zoom-release and
ring/time-scrub misses remain outside this round's closure claims.
