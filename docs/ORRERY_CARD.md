Round 2 visual fixes (2026-10-14)

All four reported defects are fixed. Shared APIs (`orrery.h`, `face_ext.h`, `ephem.h`) are unchanged. Work was limited to this worktree; this verification session performed no firmware build, device/serial access, commits or branch operations. Another process advanced HEAD to `4ec454f` during verification; those source changes were preserved.

- `nos/faces/orrery_card.c`: Saturn scales from R=88 down to R=45 at the poles, fitting the entire 2.33 R ring ellipse with a sprite margin. The same dimensions drive the sphere LUT, ring LUT and render, including in-flight tilt updates. Other globes use R=106, leaving 26 px before the distance hero anchor. Fixed sprite/LUT allocations are unchanged.
- Moon/craft names use full font bounds, planet/ring and marker exclusions, and reserved Sun/You label boxes. Outer markers get placement priority, keeping HOPE beside its marker. The diagram fits below the title/count block, including northward direction markers. Direction positions are cached in tick; draw consumes them. Added metadata is less than 1 KiB across both body caches.
- `nos/faces/orrery_card_data.inc`: shortened heat copy and removed stray encoding characters. Venus has a two-line annotation to the right of the curve; footer wrapping is limited to the lower glass chord. All 11 heat and photo pages and all moons pages were inspected, including Jupiter and Saturn.
- `tools/orrery_card/card_edges.txt`: covers both Saturn poles and an oblique tilt in PHOTO, HEAT and DOT, with idle and late-frame assertions.

Strict simulator build (`build/nos_sim_r2review.exe`) and `tests/ephem_test.c` (`-Wall -Wextra -Werror`) pass. All 12 script runs exit 0, with all `expect idle` and `expect nolate` assertions passing. PNGs and contact sheets reviewed. Results are in `build/orrery/r2_<script>/`; review sheets are in `build/orrery/r2_review/`.

| Script | Frames | Peak estimated ms | Late |
|---|---:|---:|---:|
| card_smoke | 261 | 24.5 | 0 |
| card_bodies | 2640 | 24.5 | 0 |
| card_modes | 7075 | 24.5 | 0 |
| card_motion | 4673 | 24.7 | 0 |
| card_detail | 877 | 24.4 | 0 |
| card_edges | 1002 | 24.4 | 0 |
| card_events | 393 | 24.4 | 0 |
| card_face_idle | 973 | 25.8 | 0 |
| full_face | 931 | 25.8 | 0 |
| regress | 212 | 24.4 | 0 |
| orrery1 | 563 | 24.4 | 0 |
| orrery2 | 222 | 24.4 | 0 |

Damage checks pass for bodies (2,640 frames), expanded edges (1,002), and motion (4,673): 8,315 frames identical to full repaint. Their generated scripts omit idle/nolate assertions because forced full repaint invalidates those assertions. Ordinary runs retain them. Timings remain simulator estimates, not board measurements. Existing face filter-toast width warnings remain.

Review examples: [Saturn north](../build/orrery/r2_card_edges/saturn_north.png), [Mars moons/craft](../build/orrery/r2_card_bodies/mars_page2.png), [Venus heat](../build/orrery/r2_card_bodies/venus_page1.png), [Pluto photo](../build/orrery/r2_card_bodies/pluto_photo.png).

The original implementation report below is historical. Its R=118 sizing, fixed R=88/clipped Saturn limitation, metadata sizes, and timing results are superseded above.

---

The ORRERY planet card is implemented and simulator-verified on `solar-watch-face`, based on HEAD `e6733dd`. No commits, branch changes, firmware builds, serial access or flashing were performed. The shared APIs in `orrery.h`, `face_ext.h` and `ephem.h` are unchanged.

Changed files:

- `nos/faces/orrery_card.c`: PHOTO / HEAT / colour DOT globe, NOW / FREE, real fixed-Sun geometry, Earth night map, Saturn ring lookup and shadow, heat profiles, moons and craft with information toasts, facts, vertical paging, horizontal neighbour previews, rim detents, core spin / tilt and engine-owned left-edge back. Sun and Moon have three pages; the other bodies have four. Body selection starts an iris at the tapped planet.
- `nos/faces/orrery_card_data.inc`: the reviewed prototype's heat-profile samples, axis labels and fact rows, in read-only C data.
- `nos/faces/face_orrery.c`: remove zoom-end labels that overlapped the event chip; protect the locked clock from tilted orbit lines; include ring-opening geometry and the complete date in damage hashes; hold resting ring layout at minute resolution; group guide ellipses and dashed lines to prevent recorder overflow. Controls and holds are unchanged.
- `nos/faces/orrery_events.c`: 90 px hit rows, aligned central text, wrapped titles, omit unreadable partial rows at the round edge, and remove doubled empty-state brackets.
- `sim/nos_sim.c`: include `faces/orrery.h`, register `open orrery_card`, and add `card N` (0 Sun, 1 Mercury, 2 Venus, 3 Earth, 4 Moon, 5 Mars, 6 Jupiter, 7 Saturn, 8 Uranus, 9 Neptune, 10 Pluto). This debug symbol is declared only in the simulator; no shared header changes.
- `tools/orrery_card/*.txt`: eight reproducible scripts, also present in `build/orrery/` as requested.
- `docs/ORRERY.md`, this report: implementation and verification notes.

Rendering and memory:

`draw()` does no IO, allocation or state mutation. The globe and halftone are RGB565 sprites prepared in `tick()`. One 1 MiB texture buffer is reused. Earth alone allocates the additional 1 MiB night map, freed when leaving Earth/the card. SD reads are 32 KiB per tick; short/missing files remain safe tinted fallbacks. Saturn uses its 512-sample RGBA ring map when present.

The sphere LUT stores eight bytes per pixel: longitude, latitude, texture row and normal Z. X/Y normals follow directly from pixel coordinates. Fast polynomial atan and fast sqrt build it in two slices of at most 128 rows; shading runs on the following frame. No libm call occurs per globe pixel. Saturn's signed ring-index table is rebuilt separately when latitude changes, then reused while spinning. Heat dots are grouped into small damage commands; halftone dots are baked into the sprite.

Allocated PSRAM is approximately 2.02 MiB, plus 1 MiB for Earth. Fixed sprite allocations are each 416 x 236 x 2 bytes. Sphere LUT: 445,568 bytes; ring LUT: 196,352; two body metadata blocks: 31,152; ring texture: 2,048; lighting LUT: 512. The simulator's 64-bit state struct is 424 bytes; the firmware's pointer sizes differ. The card itself peaked at 90 damage commands in the all-mode sweep. The full-face run now peaks at 259, down from 398 before guide grouping.

Cost accounting:

The stock simulator prices raster primitives but does not automatically price arbitrary work in tick. Explicit proxy charges in the card include globe shading, LUT construction, ring tests and halftone conversion. They are estimates, not measured ESP32 timings. At R=118, the charged globe work is approximately 8.31 ms PHOTO, 24.93 ms HEAT, and 13.83 ms DOT, excluding the sprite blit (about 1.84 ms), labels, bus and fixed frame costs. One 128-row LUT slice is charged up to 11.48 ms and is kept separate from shading. At R=88, Saturn's sphere alone is 4.62 / 13.87 ms PHOTO / HEAT; its visible ring band and DOT conversion add size-dependent charges. At a pole, ring-table construction is at most 12.96 ms on a separate frame. At rest there is no globe render or repaint, and the simulator reports its fixed 1.5 ms bookkeeping cost.

The maximum measured *simulator estimate* in the card motion script is 30.0 ms, with zero late frames. Actual Xtensa execution, PSRAM contention, SD read latency and total firmware memory pressure are unmeasured. Ephemeris and metadata preparation are not calibrated by these pixel proxies. The proxy values must not be quoted as board measurements.

Reproduce from the worktree root:

```powershell
New-Item -ItemType Directory -Force build/orrery | Out-Null
Copy-Item tools/orrery_card/*.txt build/orrery/
.\sim\nos_build.ps1 -Strict -Out nos_sim_card.exe
build\nos_sim_card.exe --script build\orrery\card_motion.txt --out build\orrery\card_motion
```

Run the same command for `card_smoke`, `card_bodies`, `card_modes`, `card_detail`, `card_edges`, `card_events`, and `card_face_idle`. The last script also asserts idle with the time rings open and in the beyond-Neptune view. The face scripts (`tools/orrery/full_face.txt`, `regress`, `orrery1`, `orrery2`) were run with `--face 8`, with outputs under `build/orrery/review_<name>/`.

The requested PowerShell 7 executable was denied by this environment, and its preferred WinGet GCC was inaccessible. The equivalent strict build succeeded using Windows PowerShell and `C:\mingw64\bin` prepended to PATH for the process. The card also compiled separately with `-Wall -Wextra -Werror`. Older GCC emitted warnings in existing simulator Windows/printf declarations on the initial simulator rebuild; no warning remains in the card. `tests/ephem_test.c` was compiled with `-Werror` and passes: Horizons, seven almanac dates, moons, conics and globe axes.

The scripts exercise all 11 bodies, every page in each of the three retained globe modes, NOW/FREE spins and tilts, page swipes and flicks, cancelled drags, interrupted paging, body swipes and short flicks, three queued rim detents, both FREE pole limits, rapid body changes during IO, moon-info toasts, and left-edge back. Screenshots were inspected, including Earth city lights, Venus's crescent, Moon phase, Saturn's rings, coloured halftone, heat curves and all page layouts. The prototype was reviewed from HTML/JS source; a browser-open attempt was blocked by tool approval policy.

Missing-atlas and truncated-Earth-texture runs also passed with zero late frames and idle assertions. Test moves were restored in finally blocks; all twelve original texture files are present with their expected lengths. Allocation failure and physical haptics were not injected/tested.

Damage checks compare every frame against a forced full repaint. Passing runs: full face 931 frames; all bodies 2,640; motion 4,673; detail 877; events 393. Copies named `build/orrery/check_*.txt` omit `expect idle` and `expect nolate`, which are not meaningful under intentionally forced full repaint. All corresponding ordinary-script assertions pass.

Differences and limits relative to the HTML prototype:

- Neighbour previews are correctly lit tinted spheres, using the one-buffer memory budget. They do not preload the neighbour's photo or ring texture. The incoming body loads after commitment.
- Latitude is quantised to 2 degrees; a tilt update takes roughly 100 ms, or 133 ms with a Saturn ring rebuild. Longitude-only spin renders each changed frame.
- Saturn stays at R=88. Rings at steep FREE tilts are clipped to the 236 px-high globe viewport so they cannot cover the title, distance or switch.
- Body settling also uses a bounded quadratic slide instead of the prototype's 12-frame STEADY curve. Page settling uses the specified 15 frames per full page, no spring or overshoot.
- Labels are larger and some notes wrap; facts use two lines per row, and next events show a day countdown. Moon/craft labels use collision avoidance, and their info toast is a wrapped local overlay so information is not clipped by the engine's single-line toast slot.
- The moons page centre is a simple tinted disc. Orbital guides are circles using the prototype's square-root distance scale; the rendered globe is the page with full sunlight shading.
- RGB565 and 4 px cell coverage differ from browser RGBA/subpixel antialiasing, most visibly in very dark DOT textures. Pluto remains a shaded tint.
- No firmware build, board timing, SD throughput, physical watch inspection or hardware haptics test was performed. `docs/MOTION30.html` was absent; the supplied 64 px/frame and 15-frame page budgets were used.

Representative PNGs (relative to the worktree root):

- `build/orrery/card_detail/earth_night.png`
- `build/orrery/card_detail/venus_crescent.png`
- `build/orrery/card_detail/moon_phase.png`
- `build/orrery/card_bodies/saturn_photo.png`
- `build/orrery/card_bodies/earth_dot.png`
- `build/orrery/card_bodies/venus_page1.png`
- `build/orrery/card_bodies/mars_page2.png`
- `build/orrery/card_events/events_scrolled.png`
- `build/orrery/review_full_face/f10_rings.png`
- `build/orrery/review_full_face/f21_relocked.png`

Final simulator summary lines follow (p95 includes idle frames; sending p95 is also retained). Detailed per-section results are in each output's `summary.txt` and `frames.csv`.

`review_full_face`

```text
ALL            frames   931  sent   419  est ms p50   1.5  p95  24.4  max  27.0  | sending p50   9.7 p95  24.4  | late 0 (0.0%)
```

`review_regress`

```text
ALL            frames   212  sent    40  est ms p50   1.5  p95  24.4  max  24.4  | sending p50  24.4 p95  24.4  | late 0 (0.0%)
```

`review_orrery1`

```text
ALL            frames   563  sent   287  est ms p50   2.5  p95  23.5  max  24.4  | sending p50   8.8 p95  24.4  | late 0 (0.0%)
```

`review_orrery2`

```text
ALL            frames   222  sent    89  est ms p50   1.5  p95  24.4  max  24.4  | sending p50  11.9 p95  24.4  | late 0 (0.0%)
```

`card_events`

```text
ALL            frames   393  sent   185  est ms p50   2.2  p95  24.4  max  24.4  | sending p50   9.0 p95  24.4  | late 0 (0.0%)
```

`card_bodies`

```text
ALL            frames  2640  sent   719  est ms p50   1.5  p95  18.2  max  29.7  | sending p50  15.7 p95  20.3  | late 0 (0.0%)
```

`card_modes`

```text
ALL            frames  7075  sent  2118  est ms p50   1.5  p95  18.3  max  29.7  | sending p50  15.6 p95  20.1  | late 0 (0.0%)
```

`card_motion`

```text
ALL            frames  4673  sent  1850  est ms p50   1.5  p95  29.6  max  30.0  | sending p50  18.5 p95  29.6  | late 0 (0.0%)
```

`card_detail`

```text
ALL            frames   877  sent   164  est ms p50   1.5  p95  13.1  max  24.4  | sending p50  12.9 p95  24.4  | late 0 (0.0%)
```

`card_edges`

```text
ALL            frames   793  sent   114  est ms p50   1.5  p95  13.8  max  24.4  | sending p50  12.8 p95  24.4  | late 0 (0.0%)
```

`card_face_idle`

```text
ALL            frames   973  sent   418  est ms p50   1.5  p95  24.4  max  27.0  | sending p50   9.7 p95  24.4  | late 0 (0.0%)
```

`card_smoke`

```text
ALL            frames   261  sent    31  est ms p50   1.5  p95  13.6  max  29.7  | sending p50  13.1 p95  24.4  | late 0 (0.0%)
```

`card_missing`

```text
ALL            frames  2640  sent   709  est ms p50   1.5  p95  18.2  max  29.7  | sending p50  15.8 p95  20.3  | late 0 (0.0%)
```

`card_truncated`

```text
ALL            frames   261  sent    30  est ms p50   1.5  p95  14.2  max  29.7  | sending p50  13.1 p95  24.4  | late 0 (0.0%)
```

Motion sections:

```text
earth_photo_spin frames   382  sent   166  est ms p50   2.7  p95  12.9  max  29.7  | sending p50  12.9 p95  12.9  | late 0 (0.0%)
earth_heat_spin frames   382  sent   166  est ms p50   2.7  p95  29.6  max  29.9  | sending p50  29.6 p95  29.6  | late 0 (0.0%)
earth_dot_spin frames   382  sent   166  est ms p50   2.7  p95  18.5  max  18.8  | sending p50  18.5 p95  18.5  | late 0 (0.0%)
earth_paging   frames   263  sent    96  est ms p50   1.5  p95  15.7  max  24.4  | sending p50  13.5 p95  16.1  | late 0 (0.0%)
saturn_photo_spin frames   382  sent   164  est ms p50   4.7  p95  17.3  max  20.8  | sending p50  11.5 p95  17.3  | late 0 (0.0%)
saturn_heat_spin frames   382  sent   164  est ms p50   4.7  p95  26.6  max  26.6  | sending p50  20.7 p95  26.6  | late 0 (0.0%)
saturn_dot_spin frames   382  sent   164  est ms p50   4.7  p95  27.0  max  27.0  | sending p50  18.7 p95  27.0  | late 0 (0.0%)
saturn_paging  frames   263  sent    96  est ms p50   1.5  p95  19.6  max  24.4  | sending p50  16.9 p95  21.9  | late 0 (0.0%)
venus_photo_spin frames   382  sent   162  est ms p50   2.7  p95  13.0  max  29.7  | sending p50  13.0 p95  13.0  | late 0 (0.0%)
venus_heat_spin frames   382  sent   162  est ms p50   2.7  p95  29.6  max  30.0  | sending p50  29.6 p95  29.6  | late 0 (0.0%)
venus_dot_spin frames   382  sent   162  est ms p50   2.7  p95  18.5  max  18.8  | sending p50  18.5 p95  18.5  | late 0 (0.0%)
venus_paging   frames   173  sent    92  est ms p50   6.5  p95  16.3  max  18.6  | sending p50  13.8 p95  16.6  | late 0 (0.0%)
body_swipe     frames   160  sent    51  est ms p50   1.5  p95  12.9  max  17.8  | sending p50  11.6 p95  14.6  | late 0 (0.0%)
rim            frames    83  sent    17  est ms p50   1.5  p95  12.9  max  17.3  | sending p50  12.5 p95  17.3  | late 0 (0.0%)
back           frames    37  sent     9  est ms p50   1.5  p95  24.4  max  24.4  | sending p50  24.4 p95  24.4  | late 0 (0.0%)
```
