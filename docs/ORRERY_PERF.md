# ORRERY round 2: stripe-indexed retained drawing (2026-10-15)

Implemented against the supplied `solar-watch-face` / `6c12c78` face. HEAD was
observed at `c4aeeb9` during final review; that external commit changes only
`tests/pip_eye_shape_test.c` and was preserved. This session made no commits, pushes,
rebases, flashes or serial access. No changes to `nos/nos.c`, `nos/ui.c`,
`nos/kblur.c`, `firmware/main`, the card, or the other worktree.

The PSRAM pixel canvas is gone. **Simulator regression is passing; board deadlines
are not established.** The supplied warm-full result has a material timing gap:
46 ms work versus 24 ms raster. This change cannot honestly be credited with removing
those other 22 ms without a trace identifying them. The new timers cover tick work
so the next board run can distinguish it from record/flush/drain work.

## Rendering and buckets

`face_orrery.c` captures drawing once when the scene key changes; the private
`face_orrery_draw.h` retains primitives in their original paint order. Storage is
geometry and text in PSRAM, never a PSRAM pixel target. There is no face clear or
face pixel rendering in tick, and no full-canvas copy in draw.

- Each 36-byte non-belt command has a type, parameters, ink, first/last 16-row
  stripe and conservative horizontal bounds. Stack text is copied into a bounded
  text arena. Belt/Kuiper entries are compact indices into the existing projected
  arrays: a paint-order sentinel inserts them between guides and trails. This avoids
  duplicating about 144 KB of dot commands on every rebuild.
- A counting/prefix-sum pass builds 30 contiguous ranges of uint16 command indices.
  Indices within a stripe preserve insertion/paint order. Draw enters that stripe's
  range directly: it never filters the complete scene list.
- Orbit hairlines are individual segments; belt/Kuiper bodies are individual indexed
  dots, and planetary trail stamps are individual dot commands. Comet tails, craft marks, planet discs/rings/drop lines,
  labels, ring ticks/labels, menu icon strokes and zoom segments are separately
  retained. Large genuine primitives, such as a whole ring, belong to every stripe
  they intersect. Controls are not retained as full-screen callback loops.
- Narrow transition columns reject entries by cached x bounds before primitive
  setup. Buffers crossing a bucket boundary are split into disjoint row views, so
  no pixel is blended twice. Logical coordinates, translation, column stride and
  clip windows are preserved. Horizontal Wu lines also limit their x traversal.
- `prog_arc_segment()` draws one segment with exactly the original `prog_arc`
  arithmetic; the card and existing progress callers retain their old path.
- Dimming happens after composition in the **internal stripe**, before live clock,
  hold and unread overlays. This preserves the former sprite's RGB565 rounding.
- Awake damage remains one stable scene command plus the original centre commands
  where necessary; the settled clock, hold and unread retain independent damage.
  AOD retains eight planet bounds and the time bounds. Record replays this small
  retained list without traversing the solar objects.
- Allocation failure or list/text/index overflow falls back to direct drawing;
  it does not silently drop objects. That path has no performance guarantee.

The temporary arc/text bound recorder reuses `S.scene_rec` during capture, before
replacing it with final damage. There is no 6.4 KB recorder on the task stack and
no 6.4 KB memset per captured label.

## Layout and pan

Geometry has a key separate from control ink and clock text. Labels have a geometry,
pan and reserved-chip-width key. Menu geometry has its own animation/selection key;
ring calendar layout has time, offset and timezone keys. Unchanged inputs skip these
passes entirely, including minute-clock updates on the settled locked face.

AU-to-radius maps are lazily cached **per view**, independently for orbital geometry
and belt positions. Zoom/tilt reuses them; changes to the relevant astronomy epoch
invalidate them. A zone morph reads the two cached maps and interpolates.

Pure pan shifts projected planets, belt, small bodies, comets/tails, craft and trails.
Orbit points retain their three pre-pan projection terms, then apply the new origin
with the original subtraction order. This avoids accumulated hairline rounding while
requiring no trig or radial mapping. Geometry outside the glass is retained; buckets
are rebuilt on every changed pan so newly exposed edges are correct. Label collision
is deliberately rechecked when pan changes because the glass boundary and clock/chip
reservations do not move with the sky. Ring/menu layout does not rerun for pan alone.

Layout uses fast/refined sqrt instead of hypot and normalized lighting vectors instead
of atan2 followed by sin/cos. Existing `fk_sin`/`fk_cos` are retained. Belt and trail
angle reduction no longer uses fmod in the hot loops. Double ephemerides remain intact;
the few gesture/zoom log/pow calls and astronomy libm have not been removed globally.

## Cost proxy: now exposes the old design

Internal stripe pixel prices are unchanged. For a PSRAM destination:

| Work | Additional charge |
|---|---:|
| Every covered destination pixel | 56 ns |
| Covered read/modify/write pixel | another 744 ns |
| Full clear, including the existing 4 ns internal fill cost | 60 ns/px total |

A 466 x 466 clear is therefore 13.029 ms, matching the supplied ~13 ms. Filled circle
and pill interiors pay only the write charge; the actual AA rim counts are separate.
Other AA classes conservatively treat covered pixels as RMW. The 800 ns aggregate
surcharge for scattered AA work includes cache effects; it is not a claim that every
physical store has this latency.

`sprite_fb()` marks the simulator-only destination field. `gfx_row()` communicates it
to existing hooks, including UI text; the direct-pointer clear path marks it explicitly.
A hand-built PSRAM framebuffer must set `cost_psram=1`; raw memory writes without cost
hooks are not measured. The board has neither that field nor these hooks/counters.

Tick construction and PSRAM-target raster costs are **serial before** stripe transfer,
not hidden behind a flush which has not started. Legacy geometry visits still cost
1 us. Retained index fetch/x-test/dispatch is a distinct 0.25 us class; raster primitive
setup and pixels remain additive. That distinction reflects the removal of callbacks,
geometry calculation and bounds reconstruction, but still needs board calibration.

A controlled build of the committed PSRAM face, linked with the new engine/proxy
objects, estimates frame 146 of `full_face` at **81.252 ms**, before layout/astronomy:
approximately 13.03 ms clear + 38.77 ms scene drawing + 5.10 ms geometry-visit allowance
+ 24.35 ms stripe/transfer pipeline. Adding the supplied 7.5 ms layout gives 88.75 ms,
inside the reported 84-90 ms rebuild range. Its full_face run has **289 late frames**,
maximum **103.474 ms**. The former design no longer passes as cheap.

This remains a coarse proxy. Ephemerides, event scanning, Kepler solving, layout,
PSRAM metadata traffic and arbitrary raw writes are not individually calibrated.
The index/build allowances must not be mistaken for measured device timings.

## Board expectations, with explicit uncertainty

These are engineering estimates for normal awake rendering, **not measured results**.
Use roughly 24-28 ms for the internal-stripe raster/transfer pipeline: the existing
copy pipeline was 24 ms (11 ms CPU + 13 ms waiting), and the new raster removes the
full-copy traffic but adds approximately 10-16 ms of useful raster/index CPU that can
overlap transfer. Preparation before the first stripe cannot overlap it.

| Scenario | Working expectation (ms work) | Basis / limitation |
|---|---:|---|
| Full repaint, retained scene | 24-28 **if outside-raster work is small** | No layout/capture/bucketing; only indexed stripe raster. If the reported extra 22 ms persists, expect roughly **44-50**, not <=28. |
| Pan | about 31-32; plausible 28-36 | From 87: remove 13 clear, about 38 PSRAM drawing and 7.5 old layout; add about 1 shift/labels and 2-3 capture/buckets. Stripe pipeline assumed comparable. |
| Zoom, warm view maps | about 32-34; plausible 29-36 | Same removal; allow 2-4 ms projection/labels instead of the old 7.5, plus capture/buckets. Cold view maps can cost more. |
| Time scrub | about 33-38; heavy/cold cases potentially 40+ | Same rendering savings, but changed AU values invalidate radial maps and Kepler/conic/orbit updates are still real tick work. No board measurement separates these yet. |
| Wake after a short sleep | about 29-32; plausible 27-35 | From the old ~82 ms upper frame, remove clear/PSRAM draw/old layout and add new preparation. Long sleep/cold astronomy has additional work. |
| Steady hold / minute clock | about 2-5 | Small damage, retained scene; minute test sends 4,248 px. Transition frames use the changed-scene budget. |
| Rest / settled AOD | zero pixels sent | Idle assertions cover every frame. Background tick CPU is not claimed to be zero. |

The ordinary pan/wake targets look plausible; **hard <=33 ms zoom/scrub and <=28 ms
full-work targets are not proved**. In particular, it would be misleading to subtract
PSRAM rebuild cost from a frame reported to have reused its scene. The unexplained
warm-full 22 ms could include update/event scan, recording, prewarm or drain work;
none is assumed free. Read the new face timers together with NOSF `rec`, `ras`, `wait`
and `drain` on the next bench run.

## Permanent board stage timers

Only `#ifndef GFX_COST_MODEL`; microseconds accumulate into the host-reset counters:

| NOSF field | Slot | Face work counted |
|---|---:|---|
| `u` | 0 | Tick update before scene_update: time, astronomy/belt, events, animation, chip |
| `o` | 1 | Geometry projection or pan shift, label collision, ring/menu layout |
| `sel` | 2 | Capture non-belt primitives and text; **no pixels drawn** |
| `clr` | 3 | Bucket construction (including belt indices) and retained damage recording; **no canvas clear** |

Unchanged scenes add zero to slots 1-3. Initial state/list allocation and a small amount
of key bookkeeping are outside these stage intervals. NOS already uses the same four
counters for transition composition: when `tx!=0`, its timings are additive. Inspect
settled HOME frames for isolated face stages; do not label transition totals as purely
face work. No excluded engine or firmware source was edited to change this behaviour.

## Verification

- Strict cost and no-cost host builds pass. Existing older-MinGW platform warnings
  remain; there are no new face warnings. Simulator hash I/O enables MinGW C99 stdio
  so `%llx` round-trips rather than producing an empty/misread damage comparison.
- All **14 original scripts**, **21,106 frames**, exit **0**, with **0 late frames**
  in an independent CSV count. Maximum estimate is **32.472 ms**, including launcher
  prewarm at startup. Ordinary changed-face frames are around 28-29 ms in this proxy,
  which still excludes separately calibrated astronomy/layout costs.
- Scripts: full_face, regress, orrery1, orrery2, zoom, idle; card_smoke, card_bodies,
  card_modes, card_motion, card_detail, card_edges, card_events, card_face_idle.
- Advancing-RTC `expect idle 5000` passes for locked, AOD and wake intervals. The clock
  minute sends exactly one 4,248-pixel frame, estimated 2.938 ms; subsequent idle passes.
  Section totals include transition setup after the idle assertion and are not the
  counts for the asserted idle interval.
- Retained damage versus forced-full: **21,106 / 21,106 frames identical**, across all
  14 scripts. Check copies replace duration assertions with equal-duration waits;
  ordinary scripts retain every assertion.
- **298 / 298 native 466 x 466 screenshots** match the committed PSRAM face linked
  with the same engine objects, including picker dimming. This includes the 21 full-face
  shots and 235 card/events shots (plus other face scripts). Contact-sheet timing
  captions are excluded from pixel comparisons. This is sampled visual parity, not a
  claim of bit identity over every possible continuous pan/time trajectory.
- **33 / 33** native full_face/orrery1 images match cost versus no-cost builds.
- `nos_build.ps1 -Verify`: **400 random primitive scenes** are byte-identical to HEAD;
  covered-pixel accuracy checks pass for fill, circle, pill, ring, arc, line, text and blit.
- Card source and shared astronomy APIs are unchanged. No allocation-failure injection,
  capacity-exhaustion test, ESP-IDF firmware link or physical watch measurement was done.

## Memory and artifacts

The 434,312-byte pixel canvas is removed. Host-sized retained list capacity is 387,216
bytes (8,192 commands, 32,768 uint16 references, 256 text slots). State is about 83.6 KB,
including per-view orbit radii and pre-pan orbit terms. Each visited view lazily adds
15,996 bytes of belt radii for 3,999 bodies: 63,984 bytes for all four views. Compared
with the committed state plus canvas, this is approximately 2.2 KB less for one cached
view, or 44.7 KiB more after all four are cached. Firmware pointer/alignment sizes differ.
All large allocations remain PSRAM; bucket construction uses two 30-entry stack arrays.

Artifacts under `build/orrery/`:

- `round2_<script>/`: final CSVs, summaries, images and logs.
- `round2_metrics.json`, `round2_damage.json`: frame/late/exit and damage results.
- `check_round2_<script>/`: forced-full comparison hashes and logs.
- `round2_control_<script>/`: controlled committed-face screenshot baseline.
- `round2_psram_final_full_face/`, `round2_psram_final_orrery1/`: old design/new proxy.
- `round2_nocost_full_face/`, `round2_nocost_orrery1/`: no-cost visual checks.

Reproduce the builds and ordinary runs (no board access):

```powershell
$env:PATH = 'C:\mingw64\bin;' + $env:PATH  # only if gcc is not already discoverable
.\sim\nos_build.ps1 -Strict -Verify -Out nos_sim_round2.exe
.\sim\nos_build.ps1 -Strict -NoCostModel -Out nos_sim_round2_nocost.exe
Get-ChildItem tools/orrery/*.txt, tools/orrery_card/*.txt | ForEach-Object {
    & build/nos_sim_round2.exe --face 8 --script $_.FullName --out "build/orrery/round2_$($_.BaseName)"
    if ($LASTEXITCODE) { throw "Failed: $($_.Name)" }
}
```

`build/run_round2.py`, `build/check_round2.py`, `build/round2_results.py` and
`build/round2_nocost.py` contain the session's CSV, damage and image comparisons.
