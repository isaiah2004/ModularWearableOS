# Ultra — six rounds of reference review

2026-09-14. This continues the curved weather/health layout. SVG and motion-data
work remain deferred as requested. The user asked for at least five cycles;
**six actual edit → rebuild → render → nine-tile inspection cycles** are saved.

[Final dial](assets/ultra-review/pass-6.png) ·
[Final 3×3 grid](assets/ultra-review/pass-6-grid.png) ·
[Starting dial](assets/ultra-review/baseline.png) ·
[Reference crop](assets/ultra-review/reference.png) ·
[Reference 3×3 grid](assets/ultra-review/reference-grid.png)

## How the comparison was made

Every pass was compiled from `core/apps/face_ultra.c`, rendered as RGB565 at
466×466, and split into nine tiles. The contact sheets enlarge the pixels using
nearest-neighbor scaling so overlaps and edge quality remain visible. Each pass
also generated four second-hand positions. The primary comparison uses 10:09:23,
72% battery, 6,234 steps and day 12 consistently across all six edits.

The reference crop includes its physical bezel and has been resized to a square
for layout comparison. This is **not perspective correction**; it is not a valid
pixel-difference or exact color target. The original photo guided shapes and
relationships; the native-size render guided readability.

Tile positions: R1 = upper, R2 = middle, R3 = lower; C1 = left, C2 = center,
C3 = right. All nine tiles were inspected after each edit, including regions
that did not change, to look for regressions.

## Baseline diagnosis

[Starting 3×3 grid](assets/ultra-review/baseline-grid.png)

| Tile | Finding against the reference |
|---|---|
| R1 C1 | Step number follows the curve, but its tiny detached symbol gives little context. Step stroke is thin. |
| R1 C2 | Rounded Saira 12 differs from the angular reference; the orange index resembles a pin. Battery icon is horizontal. |
| R1 C3 | Battery dashes float without a dark recessed track. Minute hand is a narrow needle. |
| R2 C1 | Footprint touches the top of 9. There is no clear separation between the numeral and the step complication. |
| R2 C2 | Long hand tails form a rotor around the hub; tiny clouds and low-contrast grid reduce the forecast's presence. |
| R2 C3 | Date is narrow and rounded; sun looks like a flower and sits too close to the date. Heart is a diamond-like blob. |
| R3 C1 | Score well is undersized, with an almost invisible remainder track. |
| R3 C2 | Rounded 6 disagrees with reference geometry; the center and score have inconsistent visual weight. |
| R3 C3 | Lower gauge is too short and has no recessed track. Bezel/tick separation is otherwise sound. |

## Pass 1 — step identity and clearance

[Render](assets/ultra-review/pass-1.png) · [3×3 grid](assets/ultra-review/pass-1-grid.png)

Replaced the toe-dot footprint with a side-view shoe, including heel, ankle,
toe and sole. Moved it from angle −1.40/radius 90.5 to **−1.23/radius 80**;
the step number moved to angle −0.85. Orange connects the icon to its gauge.

| Tile | Review after the edit |
|---|---|
| R1 C1 | Value remains within its curved lane; moving its midpoint creates space for the shoe ahead of it. |
| R1 C2 | 12/battery untouched; typography mismatch remains a later priority. |
| R1 C3 | Battery and minute-hand relationship unchanged; no new clash. |
| R2 C1 | Shoe is visibly separated from 9. It now belongs to the readout, rather than sitting on the numeral. |
| R2 C2 | Center still dominated by solid hands and long tails; next priority. |
| R2 C3 | Date/sun/heart issues remain; no new overlap from the step fix. |
| R3 C1 | Score unchanged and still small against the reference. |
| R3 C2 | Lower arcs and 6 retain their spacing. |
| R3 C3 | Lower tick bank and rotated 04/05 remain separated. |

Decision: retain the shoe placement; add explicit step wording if needed after
the rest of the hierarchy settles.

## Pass 2 — reference hand silhouette

[Render](assets/ultra-review/pass-2.png) · [3×3 grid](assets/ultra-review/pass-2-grid.png)

Replaced the needles with chamfered white frames, dark channels and short hub
stems. Hour/minute frames run from radius 13 to 61/103. Removed their rear tails.
The second hand now has a fine 0.8-unit needle, pointed counterbalance and an
orange hub with a dark center.

| Tile | Review after the edit |
|---|---|
| R1 C1 | Step value remains unobscured at the showcase time. |
| R1 C2 | Noon index and 12 still need angular treatment; hand edit introduces no top collision. |
| R1 C3 | Broad minute frame now resembles the reference, with a defined tip instead of a needle. |
| R2 C1 | Hour frame has clear edges over the forecast. Shoe/9 clearance remains intact. |
| R2 C2 | Short stems remove the rotor effect; orange hub and counterbalance read as one mechanism. |
| R2 C3 | Minute channel stays dark over the dial. Normal moving-hand occlusion remains expected. |
| R3 C1 | Score is no longer competing with a white rear hand tail. |
| R3 C2 | Fine second needle reduces the line's visual weight across the lower area. |
| R3 C3 | Needle crosses the health/gauge area cleanly; static elements remain separated. |

Decision: retain framed hands and use the reference's angular shape language
for the large digits next.

## Pass 3 — angular numerals and noon index

[Render](assets/ultra-review/pass-3.png) · [3×3 grid](assets/ultra-review/pass-3-grid.png)

Added small, scalable vector outlines for **12/9/6**, using chamfered corners,
rectangular counters and consistent stroke proportions. Saira remains for values
and labels. Replaced the pin-like noon marker with a flat triangular outline
and a white hour tick. Minor minute markers became slightly longer/brighter.

| Tile | Review after the edit |
|---|---|
| R1 C1 | Rotated bezel numbers and live step value keep their font and clearance. |
| R1 C2 | Angular 12 and the triangle are substantially closer to the reference's instrument forms. |
| R1 C3 | Minute frame and battery bank still have distinct boundaries. |
| R2 C1 | Rectangular 9 counter is legible; shoe still sits clear above it. |
| R2 C2 | New cardinals balance the chamfered hands without adding central content. |
| R2 C3 | Narrow rounded date lettering is now more visibly inconsistent; reserve a finishing pass for it. |
| R3 C1 | Score retains clear spacing from the reshaped 9. |
| R3 C2 | Angular 6 retains the lower health band's clearance; its counter remains open. |
| R3 C3 | Bezel 05 and lower markers remain independent of 6. |

Decision: keep the new hero numerals; address complication recognition next.

## Pass 4 — forecast and gauge detail

[Render](assets/ultra-review/pass-4.png) · [3×3 grid](assets/ultra-review/pass-4-grid.png)

Forecast labels increased from 7 to **8 grid units**. Clouds grew, with a dark
outline separating them from the grid; rain uses five varied strokes. Rebuilt
the heart from two rounded strokes. Added dark 9-unit recessed gauge tracks,
widened the step stroke to 5.5 units, rotated/recolored the battery symbol and
replaced the flower-like weather icon with an eight-ray sun. The lower bank now
spans **1.96–2.84 radians with 19 ticks**, leaving a dedicated sun gap.

| Tile | Review after the edit |
|---|---|
| R1 C1 | Stronger step arc is closer to the reference and remains clear of the readout. |
| R1 C2 | Larger forecast labels fit the fan; battery's upright orange silhouette belongs with the gauge. |
| R1 C3 | Dark gauge recess organizes the orange dashes instead of leaving them floating. |
| R2 C1 | 10 AM remains inside the fan; shoe and 9 do not touch. |
| R2 C2 | Weather symbols are more visible, but cloud symmetry still looks too pointed; revise their lobes. |
| R2 C3 | Heart has a visible notch; sun is recognizable and has space between date and lower gauge. |
| R3 C1 | Score's size and weak remainder track now stand out against the stronger outer gauges. |
| R3 C2 | Lower gauge extends toward 6 while preserving a gap from the digit. |
| R3 C3 | Longer bank fills the reference-like arc; unlit ticks and bezel numbers remain separate. |

Decision: correct the remaining hierarchy and RGB565 color issues.

## Pass 5 — hierarchy, color and score

[Render](assets/ultra-review/pass-5.png) · [3×3 grid](assets/ultra-review/pass-5-grid.png)

Made the dial **neutral charcoal** after RGB565 quantization, with truly black
wells and neutral white digits. Increased grid/minute-marker visibility and
used the reference's stronger orange. Added a small curved **STEPS** label.
Made cloud lobes asymmetric. Enlarged the score well to radius 28, its arc to
23.5, strengthened its amber remainder, enlarged its text, and added the small
gauge symbol in the opening.

| Tile | Review after the edit |
|---|---|
| R1 C1 | STEPS makes the value's meaning explicit. It fits between the number and forecast boundary. |
| R1 C2 | Neutral charcoal removes the previous green cast; 12 remains dominant over forecast text. |
| R1 C3 | Orange/black dash contrast is clearer; the bezel remains quieter than the hands. |
| R2 C1 | Shoe-to-9 separation remains strong, with the label resolving the semantic ambiguity. |
| R2 C2 | Asymmetric clouds read more naturally; grid and central tick scale are visible without taking over. |
| R2 C3 | Date shape/font is the remaining conspicuous mismatch; sun now has its own space. |
| R3 C1 | Larger score and amber remainder fill the quadrant more like the reference. |
| R3 C2 | Score icon fits the arc opening; 6 and the health band retain a gap. |
| R3 C3 | Lower bank remains separated from 04/05 and from the health text. |

Boundary review at 0/100% battery, 0/100,000 steps and days 1/31 found no static
collision. It highlighted the date's narrow lettering, motivating one more pass.

## Pass 6 — date finish and final checks

[Render](assets/ultra-review/pass-6.png) · [3×3 grid](assets/ultra-review/pass-6-grid.png)

Replaced the rounded date case with two chamfered vector shapes. Used the wider
Saira Bold date at cap height 11.5 instead of Condensed at 13, keeping it within
the same window. This gives it visual consistency with the hands and large digits.

| Tile | Final review |
|---|---|
| R1 C1 | Step shoe, count and label have distinct roles and remain within their allocated band. |
| R1 C2 | 12, triangle, battery and forecast label have separate silhouettes. |
| R1 C3 | Framed minute hand, black gauge recess and rotated bezel number are distinguishable. |
| R2 C1 | Shoe/9 collision is removed. Automated pixel measurement is saved in `build/ultra-review/spacing.json`. |
| R2 C2 | Short hand stems and the orange hub preserve the circular instrument center. |
| R2 C3 | Wider date and chamfered case match the face's geometry; sun remains clear below. |
| R3 C1 | Score fits its well; percentage, status and gauge symbol are separately readable. |
| R3 C2 | Angular 6 and curved feels-like text remain separated; second-hand crossings are normal analog behavior. |
| R3 C3 | Long tach bank and its dark track remain inside the rotated numeral lane. |

## Verification and limits

- Native-size renders and 3×3 grids are saved for the baseline and **all six**
  versions. Source snapshots and nine individual tiles per pass are under
  `build/ultra-review/`.
- [Reference values](assets/ultra-review/reference-values.png): 3,457 steps,
  100% battery and day 28, matching the photo's displayed data. This is a preview
  configuration, not a change to live sensor values.
- [Boundary render](assets/ultra-review/boundary.png) and
  [zero values](assets/ultra-review/zero.png) use different hour/minute positions
  as well as data extremes. These captures were made before the final date-only
  finishing pass; date changes are covered again by the final render tests.
- Final checks include cache invalidation, full-frame/stripe equivalence,
  translated swipe composition, inward text orientation and a bounded damage
  command count for the framed hands. See `build/ultra-review/tests.log`.
- Simulator/firmware build logs: `build/ultra-review/simulator.log` and
  `build/ultra-review/firmware.log`.
- A host-only full-frame benchmark during pass 5 measured about **3.90 ms cold /
  0.55 ms warm**, versus **3.02 ms / 0.39 ms** for the starting version. The framed
  hands and extra detail cost more. These desktop CPU timings are not ESP32
  performance estimates; on-watch latency and power still need measurement.
- The reference has a physical bezel, photographed surface texture and a
  proprietary typeface. This implementation approximates those visual forms
  at 466×466; it is not a pixel-perfect reproduction of the photo.
- Time/date, battery estimate and steps remain wired to the HAL. Weather,
  health, score and decorative compass are still documented demonstration
  values. No motion widgets, SVG runtime or firmware flash were added.

Reproduce a review grid with:

```powershell
./tools/preview_ultra.ps1 -OutputDirectory build/ultra-review/new
python tools/ultra_review.py build/ultra-review/new/preview2.png build/ultra-review/new/tiles
```
