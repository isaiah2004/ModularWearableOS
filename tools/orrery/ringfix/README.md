# Ring read-out review: 6b4efd0 to ringfix

Run from the repository root (Python/Pillow and MinGW at `C:/mingw64/bin`):

```powershell
python tools/orrery/ringfix/review.py
python tools/orrery/ringfix/raster.py
```

The first command rebuilds an isolated 6b4efd0 baseline plus current cost and
no-cost simulators, renders both variants, runs every `tools/orrery/**/*.txt`
script, and checks late-frame counts and pixel equivalence. It writes everything
to `build/orrery/ringfix/`. The raster runner executes the existing renderer
regressions and independent sector/input tests with cost hooks on and off.
Both runners suppress inherited Windows crash dialogs; assertions print and
exit nonzero. They do not use a device or change Git history.

Temporary simulator script commands:

- `ringplate A`: default; `[NOW]` or `[+99 D]` under DATE, separate NOW return
  under TIME only while travelled.
- `ringplate B`: `[NOW]` at now; while travelled the date status becomes the
  tappable `+99 D · NOW` capsule. Its separator dot is red.
- `ringstyle 5` remains a compatibility no-op.

Both variants use the same enlarged white plate, dark mono labels and matching
2.5 px dot-matrix numerals. The third row has its own hit area, below the digits.
The switch is Windows-only; firmware retains A pending the user's choice.

Highlights are square-ended annular sectors at each selected label's angle.
The rings move inward 6 px; their 22 px bands retain 3 px separation. A small
rim caret replaces the old large index. While rings are open, unread status
uses a short rim tick instead of the dot/backing circle that erased OCT.
Closed/locked/AOD unread rendering is unchanged. Scrubbing still moves both
sector and curved text from the same selected angle.

Three small per-band coverage caches retain radial roots, solid spans and AA
edges. Tick capture prepares the geometry, with explicit CPU costs; stripe
replay fills solids and blends only edges. Curved glyph caching is unchanged.
Ticks hidden completely by the selected segment are omitted. No cost-model
prices or late-frame thresholds were changed.

Deliverables:

- `build/orrery/ringfix/sheet.png`: A/B rows; now, +99 D, mid-drag, 3D tilt columns.
- `highlights_2x.png`: before 6b4efd0 and after, at native 2x pixel scale.
- `plates_2x.png`: A/B, now/travelled plate crops at 2x.
- `highlights_{before,after}_{now,travelled,drag}_2x.png`: individual detail crops.
- `plate_{A,B}_{now,travelled,drag,tilt}_2x.png`: individual plate crops.
- `REPORT.md`, `costs.json`, `validation.json`, `memory.json`: results.
- `review.log`, `raster.log`, `raster_{cost,nocost}.log`: validation logs.

The 396-frame sequence remains at 103 late frames for both variants (same as
6b4efd0). Resting rings: 0/30 late, 1.500 ms. Individual timings are not uniformly
lower: opening maximum grows 1.199 ms and drag median about 0.23 ms; tilt maximum
improves 0.240 ms. These are the existing re-priced model's estimates, including
new preparation work, not device measurements. The target compile-only size
probe reports 19,736 added PSRAM bytes (19,704 retained geometry, 32 state).

All 14 input scripts exit 0; cost/no-cost, damage/full, locked/AOD, runtime
switching and both positive/negative NOW returns are checked. Sector tests use
independent subpixel circle/ray membership, plus translated/dimmed/clipped
stripe replay. The scripts assert that no section's late count exceeds baseline.
