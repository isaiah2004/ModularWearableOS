# Ultra watch face — current handoff

Updated **2026-09-15**. Target: Waveshare ESP32-S3-Touch-AMOLED-1.43-B,
466×466 round RGB565 panel. Ultra remains face **4** in the gallery.

## Latest: reference detail correction

**Live data follow-up (2026-09-15):** Ultra now uses Wi-Fi forecasts and feels-like
temperature, daily persisted steps, the screen brightness setting and qualified
battery state (`USB`/`--` when a percentage is unavailable). The SpO2 marker is
square; missing optical measurements display `--`. See [Live data](LIVE_DATA.md)
for implementation, device checks and the proposed IMU widgets. This supersedes
the demo-data descriptions below.

The user's closer reference review supersedes the earlier decisions to use
dark hand channels, a STEPS label and a shoe next to the count. Hands now have
transparent apertures, flared shoulders, tapered rails and shaded bevels.
Square-ended dial marks vary in length/width; N/E/S/W replace central ticks.
The forecast reaches radius **70**, with a charcoal grid inside a black label
band. STEPS is removed, the shoe precedes the arc on its radius, and the battery
has a chamfered body with an attached terminal. Small contact shadows add depth.

[Current preview](assets/ultra-detail.png) ·
[Reference values](assets/ultra-detail-reference-values.png) ·
[Detail review and validation](ULTRA_DETAIL_REVIEW.md)

Full host tests, simulator capture/build and ESP-IDF firmware build passed.
New tests compare both hand apertures against the actual cached artwork at
three times. Following the user's simulator approval, the exact build was
flashed to the confirmed watch on **COM3** on 2026-09-15. All three segment
hashes verified; reboot reached the watch OS with Ultra (face 4) saved. The
previously unset clock was synchronized to IST and remained valid after reboot.
Before/after sources, captures, flash and boot logs, and USB validation are
under `build/ultra-detail`. `dist/v1` has not been repackaged.

## Previous: six screenshot refinement passes

Completed six edit/render/3×3-grid review cycles against the supplied photo.
See [the full nine-tile reviews for every pass](ULTRA_VISUAL_REVIEW.md).

- The step badge is now an orange shoe next to the value, with a curved STEPS
  label. It has **16 empty pixel rows** of vertical clearance from the large 9
  in the final deterministic screenshot.
- Hands use chamfered white frames, dark channels and short hub stems; the
  second hand has an orange hub and pointed counterbalance.
- Large 12/9/6 are custom cached vector outlines with chamfered corners. Saira
  remains for values, curved labels and bezel numerals. The date window also
  uses chamfered shapes, with a wider Saira Bold value.
- Larger forecast icons/labels, clearer heart/sun/battery symbols, recessed
  gauge tracks, a longer lower tick bank and a larger score well follow the
  reference more closely. RGB565 background/wells are neutral charcoal/black.
- Orange now includes the small source icons that belong to the reference's
  gauges. The earlier blanket prohibition on orange complication icons is
  superseded by the user's request to match the supplied reference.

[Latest render](assets/ultra-review/pass-6.png) ·
[Latest 3×3 grid](assets/ultra-review/pass-6-grid.png)

Source snapshots, grids, individual tiles and validation logs are retained in
`build/ultra-review`. Use `tools/ultra_review.py` to split future screenshots.

## Curved-section revision

The original reference photo has now been supplied. Per the user's preference,
the new sections retain the reference-style **weather and health** content;
accelerometer and gyro widgets are deferred.

- A recessed **upper semicircle, radii 30–68**, carries a five-slot forecast:
  tangent time labels at radius 58, weather icons at 43, and a fine radial grid.
- Two **lower-right annular sections, radii 32–47 and 50–68**, carry the heart
  rate and feels-like temperature. Text follows the curve with its tops pointing
  toward the hub, so it reads upright along the bottom-right of the dial.
- A central instrument disk and a larger lower-left score well reproduce the
  reference's partitioned layout. Steps/battery labels also follow their arcs.
- `pathtext_arc_inward` complements the existing outward-facing bezel text.
  Both remain raster-only and use the existing cached dial.
- No new sensor/data source was added; weather/health/compass remain demos.

[Latest curved preview](assets/ultra-curved.png) ·
[Boundary values](assets/ultra-curved-boundary.png) ·
[SVG layering cost assessment](SVG_RENDERING.md)

Original files for this revision are saved in `build/ultra-curves-backup`.

## Earlier typography and spacing pass

- Replaced Barlow with **static Saira Bold** for upright 12/9/6 and values,
  and **static Saira Condensed SemiBold** for rotated bezel numbers and small
  labels. Removed synthetic double-stamping of the large numerals.
- Separated overlapping bezel elements into radial lanes: gauges at **87–94**,
  numeral baselines at **101** (cap height 8.5), minute/hour ticks at **112–118**,
  on the 240-unit grid. The steps arc is now upper-left beside its readout.
- Arc glyphs rotate around their middle; text uses kerning and cap-height sizing.
- Battery icon sits beside its value. Smaller date border and complications
  leave more space around the large digits. Long step counts scale to fit.
- **6,234 → 6,235 updates on the next frame**, without waiting for a 50-step
  boundary. Exact battery/steps/day comparisons replace the old hash; strings
  format only on changed values.
- Fixed ESP-IDF source discovery: the old configured build omitted Ultra and
  pathtext, causing an undefined `face_ultra_draw` link error. CMake now tracks
  new sources during normal configuration, while respecting IDF script mode.
- Added PowerShell preview generation that refreshes **all four PNGs**, plus
  regression checks for values, caching, stripes, swipe offsets and typography.

## Preview and build

From the project root in PowerShell:

```powershell
./tools/preview_ultra.ps1
./tools/preview_ultra.ps1 -OutputDirectory build/ultra-boundary -Hour 6 -Minute 30 -Battery 100 -Steps 100000 -Day 31
./sim/build.ps1
./tools/build.ps1
./tools/test.ps1
```

The default preview writes `build/ultra-preview/preview0..3.png` and BMPs at
10:09 with seconds 0/8/23/47. Battery/steps/date are synthetic showcase values.
The standalone preview now advances `wos_frame_id` on each rendered frame.

Run `build/sim.exe`, swipe to **Ultra**. `I` toggles the damage overlay;
`R` toggles real size; `B` changes the simulated battery. The simulator's
existing `--face 4` command captures a frame and exits.

Review images retained with this document:

- [Before](assets/ultra-before.png)
- [After](assets/ultra-after.png)
- [100% battery / 100,000 steps / day 31](assets/ultra-boundary.png)

The original typography pass used the handoff description; the subsequent
curved-section revision uses the user-supplied product photo. Samsung's
[official design page](https://www.design.samsung.com/global/contents/galaxy-watch-ultra/)
was also consulted for gauge/numeral separation; its shown variants differ
from the original described reference.

## Files

| Path | Role |
|---|---|
| `core/apps/face_ultra.c` | Layout, cached dial, live hands and value invalidation |
| `core/pathtext.c`, `core/pathtext.h` | Two-font raster-only text, kerning, arc placement |
| `core/font_saira_bold_ttf.h`, `core/font_saira_condensed_ttf.h` | Flash-resident static fonts; **32,968 bytes combined** |
| `tools/bake_ultra_fonts.py` | Repeatable static instancing and ASCII subsetting |
| `assets/ULTRA_FONTS.md`, `assets/Saira-OFL.txt` | Font provenance, rebuild instructions and license |
| `tools/preview_ultra.ps1`, `tools/face_preview.c` | Deterministic preview generation |
| `tests/ultra_test.c` | Renderer/cache regression checks, run by `tools/test.ps1` |
| `firmware/main/CMakeLists.txt` | Portable-core source discovery |

Old Barlow assets/header remain for comparison but are no longer included.
This workspace has no Git repository. Original face/text/preview/handoff files
are backed up in `build/ultra-backup`; original renders are in `build/ultra-before`.

## Rendering contract

- Coordinates use the **240-unit grid** via `GFX_S`. Angles: radians,
  **0 = noon, clockwise**.
- `pathtext_draw(fb, font, s, x, baseline, cap_height_px, color, center)` draws
  straight text; `pathtext_width(font, s, cap_height_px)` matches its metrics.
  Fonts: `PATHTEXT_BOLD` or `PATHTEXT_CONDENSED`.
- `pathtext_arc_inward` has the same arguments as `pathtext_arc` and reverses
  the tangent direction for upright lower complications.
- `pathtext_arc(fb, font, s, radius_px, mid_angle, cap_height_px, color)` lays
  glyphs tangent to the circle, tops outward; bottom numerals read inverted.
- **Sizes now mean actual capital height**, not the old nominal font height.
  Fonts cover printable ASCII. Rasterization uses 3× source oversampling and
  2×2 output sampling.
- Sensor reads latch once per `wos_frame_id`. One full-screen layer, tag **10**,
  caches the dial. Only hands draw live; pathtext is a no-op in record mode.
- Each changed shown battery/step/day value or layer eviction rebuilds the dial.
  This means one rebuild per detected step while walking: measure its cost on
  hardware before claiming smooth device performance. Do not restore silently
  stale exact step values to avoid that cost.

## Design constraints

1. Orange identifies gauges, their small source icons, the second hand and the 12 index.
   Weather/health text remains grey/white.
2. Near-black charcoal background, ghost compass rose, calm center.
3. Large cardinals and white hands dominate; complication values recede in grey.
4. Curved layout: forecast above the hub, health bands lower-right, score lower-left.
   Keep large 12/9/6 outside the recessed sections.
5. Gauges, rotated numerals and minute ticks occupy separate radial lanes.

## Data honesty

**Wired values:** RTC time/date, existing ADC-derived battery estimate, estimated
QMI8658 cadence steps. Battery percentage is not a fuel gauge.

**Demo values:** `HR_BPM=89`, `FEELS=23`, `SCORE=38`, `ACTIVE=42`, weather strip
and fixed compass rose. The board has no HR sensor, magnetometer or weather
feed. These placeholders remain explicitly commented in the face source.
See [hardware](HARDWARE.md) and the main README.

## Validation and remaining work

Passed on this Windows host:

- Four deterministic 466×466 second-hand positions and boundary-value rendering.
- Simulator build and `sim.exe --face 4` capture using the normal watch OS path.
- Full `tools/test.ps1` suite, including Ultra's same-frame sensor latching,
  one-step invalidation, 0/100% battery, 0/100,000 steps, day 31, cached moving
  hands, exact full-frame/16-row-stripe equality, translated swipe composition,
  eviction, cap height, condensed width and record-mode text no-op.
- ESP-IDF 5.5.4 firmware build: `firmware/build/watchfw.bin`, see `build/ultra-review/firmware.log` for the latest binary/partition sizes. Both Ultra modules are compiled and linked.

**Still requires the physical watch:** first-render/per-step rebuild latency,
appearance on AMOLED and the existing 90° display/touch rotation. No firmware
was flashed in this pass; `dist/v1` was not repackaged. Boot default and saved
user settings are unchanged.
