# Faces — critic round 1

Scope: the 8 faces in `nos/faces/` (registry order DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE),
reviewed on the working tree (face_glyph.c, face_inverted.c and faces.c had uncommitted edits at review time).
Sim: `build/nos_sim_crit_faces.exe`. All evidence is under `build/nos/critic/faces/` (scripts in `scripts/`).

## Verdict

The engineering is very good. Every damage check passes (rest, morph, pager, AOD/wake: 5,776 frames
identical). No frame is late. Idle faces cost 1.5–2.5 ms. AOD lit area is 1.2–5.2% with no red, and the burn-in
drift works. The bezel tick ring on GLYPH is precise to 0.23° and ±0.8 px. DOT, GLYPH and INVERTED at rest are
close to shippable Nothing faces. What holds the set back is mostly the moments between rest states, plus a
few layout collisions:
- SEGMENT's build-in replays a stale cache.
- INVERTED's wake paints near-white ghost cells on black, then flashes the whole panel white in 3 frames.
- GLYPH's wake shows a wrong time for 300 ms, and its hands cover the date for about 20 minutes of every hour.
- No face shows the unread-notification dot, even though `face.h`/`sys_home.c` hand that job to the faces.
- 6 of 8 faces show no battery or charging state.

The data-led faces (SPLIT, LIFE, TAPE) need typographic and motion fixes before they'd sit beside the
CMF Watch 3 Pro's faces. SPLIT's ragged tabular "1" and LIFE's vanishing now-dot are the main ones, and TAPE's
reels don't actually show anything.

Would it ship on a Nothing product today: DOT yes (after the shared fixes). GLYPH yes once the date moves.
INVERTED yes once the wake is fixed. MATRIX close. SPLIT, LIFE, SEGMENT and TAPE not yet.

## Issues (most severe first)

1. **[P1] SEGMENT — every build-in after the first replays stale rings.** `enter()` resets `s_shown` to 0 but the
   four quadrant sprites still hold the previous visit's fill. `cache_update` repaints them at 36 segments per
   frame, going clockwise, so the build-in is a mix of old and new arcs:
   - Wake frame 0 shows the old partial rings.
   - Frames 1–7 show old lit arcs in the left half (e.g. inner ring lit 5→7 o'clock in frame 4) while the new
     fill grows from 12.
   - On the first-ever visit, the empty track is missing in the lower-left for 7 frames.

   Evidence: `aodwake/b5_wake.png` f0–f7, `seg1/seg_build.png` f9–f15, `seg3/seg_over_build.png` f0–f6.
   Fix: don't zero `s_shown` in `enter()` when `G->ok`. The cache already matches the last shown fill, so frame
   0 is correct. Animate only from the last shown value to the new target, which reads as "what you walked since
   you last looked". When the cache is cold, raster all 180 OFF segments before the first blit (one-time, about
   27 ms in tick, behind the boot) so the track is never missing.
2. **[P1] GLYPH — the hands cover the date.** The date (`ui_label` at y=330) is drawn under the hands. Any time
   the hour or minute hand points between about 5:20 and 6:40 (and the minute hand at :27–:33 of every hour),
   "SUN 11" reads "S  11". Evidence: `extremes2/y2_0631.png`, `extremes2/zoom_glyph_date.png`. Fix: move the
   date into a 3 o'clock window (right-aligned at x=388, baseline y=239, C_SEC). When the hour or minute angle
   is within ±18° of 90°, put it at 9 o'clock instead (x=78, left-aligned). Alternative: switch it per hand
   angle between y=330 and y=150.
3. **[P1] GLYPH — the wake build-in shows a wrong time.** On wake both hands swing in from 90° and 120° behind
   over 0.32 s, so a 10:11 glance reads roughly 7:10/8:50 for the first 3 frames (`aodwake/b2_wake.png`
   f0–f3). An analog face must never show a wrong time, least of all at the moment you raise your wrist. Fix:
   drop the hand swing (hands are correct from frame 0). Keep the clockwise tick-ring fill and the
   second-dot grow. If you want hand motion, sweep at most 6° (one minute).
4. **[P1] INVERTED — wake frame 2 paints ghost cells on black, then a full-panel white flash.**
   - Digits start at `open > 0.6` while the disc radius (0.6 × 236 = 141 px) is still smaller than the time
     block's half-width (176 px). The outer ghost columns (#EFEBEF) are drawn on black at x≈59–75 and
     x≈391–407 (`aodwake/zoom_inv_f2.png`, frame 2 of `b6_wake`).
   - The disc then goes from 0 to 162 px radius in 2 frames (0, 82, 162, 198, 218, 229). At night, waking into
     ~80% white in 66 ms is a flashbang.
   - The labels pop in at `open > 0.8` and the hairline pops at `open >= 1` (frame 7), with no transition.

   Fix:
   - Gate the dot layer on `open >= 1.0f`, or clip `dot_text_*` to the disc.
   - Grow the disc linearly over 0.36 s (11 frames, about 21 px/frame).
   - Fade the labels in 4 quantised steps like DOT does.
   - Draw the hairline from the start, as a ring at `min(r, 220)`.
5. **[P1] All faces — the unread dot is never drawn.** `face_ctx.unread` is filled in, and `sys_home.c:318`
   says "the face draws its own", but no face reads it. With an unread message the face page shows nothing; the
   red dot only appears once you swipe to a widget (`pager/p0_rest_unread.png` vs `pager/pager_0.png` f9+).
   Fix: add `fk_unread(fb, c)` to face_kit: a 9 px dot centred at (233, 24) on a 20 px black disc, the same as
   home. On faces that already use red (DOT, GLYPH, LIFE, INVERTED, TAPE), draw it in C_PRI so each face keeps
   one red element. On INVERTED, use C_INK on white. Call it last in every face's non-AOD `draw()`. In AOD, use
   a 6 px C_SEC dot.
6. **[P1] MATRIX, GLYPH, LIFE, SEGMENT, INVERTED, TAPE — no battery or charging state at all.** At 4% battery
   and while charging, these six faces look exactly the same as at 78% (`extremes/m_all_b.png`,
   `extremes2/m_over.png`). Fix: add a shared `fk_status(fb, c, x, y, ink)` that draws nothing above 20% and
   not charging. At ≤20% it draws `"%d%%"` as a mono label. While charging it draws `DOT_ICON_BOLT` plus the
   value, as on DOT. Put it under each face's date/label slot:
   - GLYPH: under the moved date
   - LIFE: y=128
   - SEGMENT: replace the date row while it is active
   - INVERTED: y=166
   - TAPE: right of the timecode at y=390
   - MATRIX: bottom of the matrix (row 22, cells 10–14 lit as a 5-cell bar)
7. **[P2] All faces — waking from AOD tears the time down and rebuilds it.** AOD already shows a readable time.
   On wake, every face blanks it and rebuilds it over 6–12 frames:
   - DOT: digits gone in f0, readable around f6
   - MATRIX: one dot in f0
   - SPLIT: black for f0–f1, digits readable around f6
   - INVERTED: around f9
   - TAPE: around f7

   Evidence: `aodwake/b*_wake.png`. Apple and CMF keep the time continuous across wake. Fix: on an AOD→awake
   wake (`H.last_aod && !nos_aod`), skip the build-in for the time and morph from the AOD styling instead:
   - DOT/MATRIX/INVERTED/TAPE: grow the dot diameters from the AOD size to full size over 6 frames with
     EASE_STANDARD, quantised to 4 steps.
   - SPLIT: fade the outline to fill.

   Keep the full build-in for face switches and wakes from screen-off.
8. **[P2] SPLIT — the left edge of the stack is ragged.** The grotesk "1" is tabular. "10" starts at x=86 while
   "09" starts at x=70 (16 px). "11" reads as "1  1" (`rest/f3_1009.png`, `morph/h3_hour.png` f22–f23). A
   left-aligned typographic stack lives or dies by that edge. Fix: per-glyph left side bearing. When a cell's
   digit is '1', shift its x by −(advance("0") − advance("1"))/2, about −16 px. Keep the right digit's cell
   origin unchanged so the roll still clips to a fixed cell.
9. **[P2] SPLIT — the data column pops in and its states are weak.**
   - During the build-in the rows appear one per ~2 frames with a hard pop (`aodwake/b3_wake.png` f6–f10).
     Fade each row in 4 steps over 4 frames, staggered by 2 frames.
   - Charging shows as "BAT 100+". A trailing + is cryptic; use the dot-matrix bolt before the value, as DOT
     does.
   - Low battery is C_WHITE instead of C_PRI (#FFF vs #E8E8E8), which you can't see (`extremes/x3_a_empty.png`,
     "BAT 5"). At ≤15% make the BAT label C_PRI and the value C_WHITE in a `[ 5 ]` bracket, or use the red slot
     when HR isn't using it.
   - In AOD, "BAT 78" floats alone at (300, 296) in C_LINE2 #333 (`aodwake/m_aod.png`). It isn't aligned to any
     row of the awake layout and is nearly invisible. Drop it, or put it on the BAT row baseline (y=321) in
     C_DIS.
10. **[P2] MATRIX — the seconds ring has 68 cells, so it ticks off the beat at uneven speed.** The 4-neighbour
    edge rule yields 68 ring cells with angular spacing of 4.0°–6.7°. The head advances every 0.88 s
    (`morph/s1_sec_*`, at frames 11 and 37), out of phase with every other face's 1 Hz rhythm, and it crawls on
    the diagonals. Fix: drive the head from whole seconds, `head = (sec * nring) / 60`, so it only moves on the
    second. Better: pick 60 ring cells by nearest angle to i×6° (one per second), and dim (L=1) the 8 left over
    as a static frame.
11. **[P2] MATRIX — sparkles land inside the digit boxes.** The sparkle only excludes cells that are lit in the
    digit pattern, so mid-grey #999 cells appear in the holes and gaps of digits. In 04:20 there's a #999 cell
    inside the "2"/"0" box at around (266, 264) (`rest/m_f1.png`, `rest/f1_0420.png`). Next to white digits it
    reads as a stuck pixel. Fix: exclude rows 4–20 × columns 6–18 (digit boxes plus a 1-cell margin) from the
    sparkle, and cap sparkles at level 1 (#4D4D4D).
12. **[P2] MATRIX — the minute change is out of sync with the seconds ring.** At :00 the whole lit ring (up to
    67 cells) goes dark in one frame (`morph/m1_morph.png` f14, `morph/h1_hour.png` f14). The minute digits only
    start changing 3–5 frames later, because the MM rows are delayed `(r-5)*0.012` s, and finish at f21. Fix:
    drain the ring backwards over the same 6 frames as the digit scan (12 cells per frame, through L=1→ghost),
    and start the MM rows at 0 delay (scan within each line, not across both).
13. **[P2] LIFE — the "now" dot disappears at every minute change, and the time hard-cuts.** At hh:mm:00 the
    old red dot turns white instantly, and there is no red dot at all for 3 frames until `grow` (EASE_MECH over
    0.25 s) catches up (`morph/zm4.png`, f15–f17). The grotesk time "10:09"→"10:10" swaps in one frame. Fix:
    - Start the new red dot at 40% size in frame 0 (`grow = 0.4 + 0.6*fk_mech(since/0.2f)`).
    - Turn the old dot white over 3 frames (ui_mix in 3 steps).
    - Odometer-roll the changed digit of the small time (reuse SPLIT's `digit()` with UI_F_TITLE, 0.2 s).
14. **[P2] LIFE — the hour drain collapses, and the first dot goes dark early.** At hh:00 the grid drains 60
    dots with EASE_MECH over 0.45 s. That's 1–3 dots per frame for 4 frames, then about 25 dots (2.5 rows)
    vanish between f20 and f21 (`morph/h4_hour.png`). Meanwhile dot 0 turns dark in f14 ahead of the drain,
    because the now-dot path draws it as C_FUT. Fix: drain linearly at 6 dots per frame (10 frames). Draw dot 0
    as C_PRI until `past` reaches 0, then grow the red now-dot.
15. **[P2] TAPE — the hubs snap 180° when the build-in ends, and every step ends with a stutter.**
    - `rot = 0` during the build-in, then it jumps to `(sec % 12) * 30°` (`aodwake/b7_wake_012` → `_013`: gap
      angle 30° → 210° in one frame). Fix: compute `rot` during the build-in too, so the hubs are already at
      the right angle.
    - The 30° step is quantised to 16 levels on EASE_MECH. The last 1/16 (1.9°) arrives a frame after a hold
      (measured per-frame 2, 9, 9, 3, **0, 2**° in `morph/s7_sec_*`). Fix: quantise to 32 levels, or snap
      `k = 1` once `k > 0.93`.
16. **[P2] TAPE — the reels don't encode anything you can read.** The pack is whole rings only (radii 28/38/48/58),
    so it changes about 3 times a day (ring 58 only before 00:24, ring 48 until 08:24, ring 38 until 16:48). For
    most of the day both reels look identical, e.g. 10:09 has 2 full rings each (`rest/f7_1009.png`). The
    header says "a few times an hour". The pack rings use C_LINE #222 at 4 px, which reads as dust/moiré on
    AMOLED (`rest/zoom_reel.png`). Fix:
    - Light pack dots per dot, not per ring. With about 155 pack dots per reel, light them outer→inner on the
      supply reel and inner→outer on the take-up reel at `day * 155`, so the reels change about every 9 minutes.
    - Raise the unlit pack dots to C_LINE2 #333 at 3 px, or drop them.
17. **[P2] TAPE — the record lamp hard-blinks and pulls the date off centre.**
    - The lamp is 9 px for 500 ms and 0 for 500 ms, a hard flicker against §7 "never a hard flicker". Reuse
      DOT's `sec_dot_d` envelope (grow 100 ms, hold, shrink).
    - The date is always offset +10 px for the lamp, so for half of every second "SUN 18" sits 10 px right of
      centre with nothing to its left (`rest/f7_0000.png`). Keep the lamp position and draw a C_SURF2 9 px
      "off" lamp, so the pair always stays balanced.
18. **[P2] INVERTED — the ghost cells and labels have a magenta cast.**
    - `C_GHOSTW 0xEF5D` decodes to (239, 235, 239): R and B are 4 levels above G. Across 80 large ghost cells on
      white it reads lavender (`rest/f6_1009.png`). Use `0xEF7D` (#EFEFEF) or `0xE73C` (#E7E7E7).
    - The same 565 rounding gives `C_GHOST 0x18E3` = (24, 28, 24), slightly green, on DOT; `0x18C3`
      (24, 24, 24) is neutral.
19. **[P2] INVERTED — the page dots are inverted during the pager slide.** The page dots draw over the white disc
    with the face never getting a backdrop. The active dot (white-ish) disappears on white while the inactive
    #333 dots stand out (`dots/zdots.png`, `dots/d6_007.png`). They also sit 6 px from the hairline at y≈447.
    Fix: home should draw the dots in `C_INK`/`C_LABW` when the face reports a light background (add
    `int light` to `nos_face`). Or give the dots their black capsule whenever page 0 is white.
20. **[P2] SEGMENT — the hero time is too small, and overflow paints whole red rings.**
    - The time's cap height is 47 px (`rest/f5_1009.png`, bbox y 195–241) inside a 348 px inner ring. The
      reference layout calls for ~96 px. The face reads as rings with a caption, not a clock. Use the hero
      grotesk at about 80 px cap, move the date to ty−cap−22, and drop the values row to ty+44/ty+66.
    - The second lap is red on all three rings at once. At 21:30 the inner ring is a full red circle plus red
      arcs on the other two (`extremes2/y5_over_low.png`, `seg3/seg_over_build.png` f12+). That's far more red
      than "one red element". Show red only for the ring that most recently crossed its goal (or only steps),
      and render the other overflowed rings as a full white lap.
21. **[P2] SEGMENT — live gains take up to 4 s while the numbers jump instantly.** A data sync fills one segment
    per frame. Active minutes from 0 to 114 took 120 frames (4.0 s), while the "114" value changed in frame 0
    (`seg4/seg_live.png`). Fix: `s_shown += max(1, ceil((target - shown) / 12))`, so any gain lands within
    0.4 s.
22. **[P2] SPLIT, SEGMENT, INVERTED — complications have no pressed state.** Tapping "HR 68" shows nothing until
    the iris starts (`taps/t3_hr.png` f1–f2 identical to f0). Faces can't see `IN_DOWN`. Fix: add
    `const nos_screen *(*hit)(int x, int y, int *rx0, int *ry0, int *rx1, int *ry1)` (or reuse `tap` with an
    out-rect). Home then fills the rect with C_SURF2 (radius 16; on INVERTED use #E0E0E0) from IN_DOWN until
    IN_UP.
23. **[P2] MATRIX — the page dots overlap the grid during the pager slide.** The page dots (y≈436) land on matrix
    rows 23/24 while the face is still under them (`pager/pager_1.png` f0–f6). Fix: same as #19: a face flag
    `full_bleed` that makes home draw the black capsule from the first frame of the drag.
24. **[P3] GLYPH — hour hand colour.** It is C_PRI (239, 235, 239) next to a C_WHITE minute hand. The 6% grey
    difference reads as a tint, not hierarchy (`rest/f2_1009.png`). Make both C_WHITE (hierarchy is already
    carried by width and length), or make the hour hand C_SEC.
25. **[P3] GLYPH/INVERTED — the second dot's per-frame motion isn't perfectly even.** Steps are 0.20–0.23° with a
    0.14° frame every 4–5 frames (GLYPH, `morph/s2_sec_*`). This is consistent with dot centres snapped to
    1/4 px (0.705 px/frame → 0.75, 0.75, 0.75, 0.5). It's sub-pixel and unlikely to be visible at 326 ppi, but
    if `dot_disc` snaps, pass the full float position for this one dot.
26. **[P3] LIFE — the now-dot "breath" moves in visible steps.** The dot's diameter goes 17→23 px in 8
    quantised levels, so it changes about every 3–4 frames (red pixel counts 232, 256, 276, 300, 332, 360, 392,
    424, `morph/s4_sec_*`). That's a 1 px step at ~8 Hz, which reads as ticking on a 23 px dot. Use 12 levels,
    or make it the DOT envelope (grow and hold) once per 2 s.
27. **[P3] SEGMENT — the values row looks left-heavy.** Column centres at CX±84 are typographically correct, but
    the row's ink spans x 118–330 (centre 224, 9 px left), because "2840" is much wider than "30". Use column
    centres CX−88 / CX / CX+80, or right-align values to a shared baseline grid.
28. **[P3] SPLIT — the data column sits 5.5 px low.** Its ink spans y 159–320 (centre 239.5) against the digit
    block's 109–359 (centre 234) (`rest/f3_1009.png`). Change `row_y` base 246 → 240.
29. **[P3] DOT — the labels hug the bezel.** The date caps run from y=33 to 44 and the battery from y=421 to 432,
    i.e. 33 px from the glass edge (symmetric, with 19 px gaps to the block). That's fine in the sim; on
    hardware, check that the physical bezel doesn't shave the cap tops. If it does, use pitch 17
    (block 321 px), which frees 9 px at each end.
30. **[P3] 12 h mode (source only — not scriptable, `prefs.hour24` defaults to 1).** `fk_hh` always zero-pads,
    so 12 h shows "09" on DOT, MATRIX, SPLIT, INVERTED and TAPE. In 12 h, show a ghost cell (DOT/MATRIX) or a
    blank leading digit (SPLIT/INVERTED/TAPE) instead of "0".
31. **[P3] Not exercised.** `set bpm 150` didn't reach `svc_heart()->bpm_last` within 3 s, so SPLIT's red HR
    out-of-range state was never seen. `set steps 0` is overridden by the sim's SIM_DAY model, so 0 steps was
    tested at 04:20 and over-goal at 21:30. The TAPE file header says the hub has "three gaps"; it has one.

## What is already excellent (don't break it)

- **Damage and cost:** `--damagecheck` passes on all four scripts (`dmg_*`). 0 late frames in every section.
  Idle sending frames cost 2.5–3 ms. Full-repaint frames (pager, build-in) top out at 24.4 ms.
- **GLYPH tick ring:** 60 dots at r 217.2–218.8 with angle error ≤0.23°. The 12 o'clock white marker plus grey
  hour dots is the right hierarchy. The minute hand's click is a crisp 2-frame step (0.19, 0.94, **3.37**, 1.13,
  0.18°). The AOD outlined hands are lovely.
- **DOT layout:** the block is exactly centred (ink bbox centre 232.5, 232.5). Date and battery are symmetric
  with 19 px gaps. The red seconds dot sits on the lattice in the HH/MM gap. Its blink envelope
  (3-frame grow, hold, 3-frame shrink) is good. The dot-morph minute change (8 frames) is the house signature;
  keep it.
- **MATRIX build-in ripple** (centre-out, with a bright crest and digits lighting behind it) and its AOD (digits
  only, 1.5% lit) are very Phone (3).
- **SPLIT odometer roll:** the changed digits only, clipped to the cell, 0.3 s EASE_MECH. Correct and
  satisfying. Taps on HR/STP/TMP open Heart/Activity/Weather from the tap point.
- **INVERTED at rest and its morph:** this is the strongest Nothing statement in the set. Black dot digits with
  ghost cells on white, a red second dot riding the hairline, and a dark AOD that never shows white.
- **AOD discipline across all 8:** 1.2–5.2% lit, max level #9C9C9C, zero red pixels, and the ±4 px Lissajous
  drift is applied consistently.
- **Pager:** every face slides 1:1 under the finger with no stale pixels, and widget pages pick up the unread
  dot correctly.
