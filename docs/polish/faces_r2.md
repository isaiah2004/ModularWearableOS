# Faces: critic round 2

Scope: the 8 faces in `nos/faces/` (DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE) and the moments
around them:
- active, AOD, and the wake from AOD;
- build-in;
- the minute, hour and midnight rollovers;
- unread, battery and charging states;
- complication taps and their pressed states;
- the home pager;
- the face picker, entered by long press and from Settings;
- 12 h mode.

Faces have no per-face customise surface, and NOS_DESIGN.md doesn't specify one, so the picker is the only edit
surface.

**Build.** Sim: `build/nos_sim_faces_critic.exe`, built 2026-10-11 21:24 from `sim/nos_build.ps1` on HEAD
2805820 plus the working tree at that time. The working tree had in-flight shell-team edits in `sys_faces.c`
(the moat around the pill) and `sys_home.c`. Commits since then don't touch `nos/faces`:
- 680b81b changes only `nos/nos.c`, the iris.
- b4f6492 changes `gfx_blend565`, which only changes AA edge tint (grey instead of green-tinted).

**Method.** All frames are 30 fps sim frames. Evidence is under `build/nos/critic/faces/r2/`, and the paths
below are relative to it:
- Scripts are in `scripts/`; analysis helpers are in `tools/`.
- Frame numbers are the `NNN` in `name_NNN.png`, which is also the left index in each contact sheet.

I did not run anything on the board (COM3 untouched), so I can't confirm the "0 late frames on device" claim
myself. The sim cost model agrees with it.

## Verdict

Round 1 landed almost completely. 26 of 31 items are fixed, 1 is mostly fixed, 3 are partly fixed, and 1 (#31) is
still only partly testable:
- SEGMENT builds from the last fill shown.
- GLYPH's date moves out of the hands' way.
- INVERTED opens as a growing disc with no white flash.
- Every face now shows the unread dot and the battery/charging state.
- Every face wakes from AOD with a 0.2 s morph instead of a rebuild.
- SPLIT's edge is straight.
- MATRIX's ring ticks at 1 Hz.
- TAPE's reels now encode the day.

The engineering is still clean: `--damagecheck` passes on 3,920 frames, there are 0 late frames in every run
(sim max 24.4 ms, all bus-bound full repaints), and AOD is 1.2–5.2% lit with APL ≤ 1.1%.

What's left is again the moments between states, now one level finer:
- **Wake end.** The 0.2 s wake morph treats AOD → active as a cross-fade of the shared parts, then adds
  everything the AOD lacks in its last frame. SEGMENT puts 70% of its final light on screen in one frame. MATRIX
  (34%) and GLYPH (date, second dot and unread dot all at once) do the same.
- **Picker.** Opened from Settings, it fades the face up to full brightness and then hard-cuts to the Settings
  list. Swiping to a face last seen minutes ago replays the stale time morph while the face slides in.
- **Rollovers and 12 h.** GLYPH's date jumps between slots in one frame. TAPE swaps both reel packs in one frame
  at midnight. INVERTED hard-cuts its digits if you wake it at a minute change. LIFE and SEGMENT still
  zero-pad in 12 h while the other six faces don't.

None of these is a P0. At rest, DOT, GLYPH, INVERTED, SPLIT and MATRIX would ship on a Nothing product today.
LIFE and TAPE are close. SEGMENT needs its wake fixed first.

## Round-1 items

| # | Item | Status | Evidence (round 2) |
|---|------|--------|--------------------|
| 1 | SEGMENT build-in replays stale rings | **Fixed.** The build-in animates from the last fill shown; there are no old arcs on frame 0 or later. | `buildin/b5.png`, `aodwake/w5.png` |
| 2 | GLYPH hands cover the date | **Fixed.** The slot order is 3 → 9 → 6 → 12, so the date is never under a hand. The new jump between slots is #7 below. | `rest/f2_0631.png`, `glyph/g_slot.png` |
| 3 | GLYPH wake shows a wrong time | **Fixed.** No hand swing; the hands are right from frame 0. | `aodwake/w2.png` |
| 4 | INVERTED ghost cells on black, then a white flash | **Fixed.** The disc grows over 0.36 s with mid-grey (0x8C51) digits, and there is no full-panel white frame. | `aodwake/w6.png` |
| 5 | Unread dot never drawn | **Fixed.** It is drawn on all 8 faces, active and AOD. | `sheet_unread.png`, `unread/u*_unread.png` |
| 6 | No battery/charging on 6 faces | **Fixed.** It shows on all 8 faces, at ≤ 20% and when charging. | `sheet_ext_f0..7.png` |
| 7 | Wake tears the time down | **Fixed.** Wake is now a 0.2 s morph from the AOD layout, but its last frame pops (#2, #3, #4 below). | `aodwake/w0..7.png` |
| 8 | SPLIT ragged left edge | **Fixed.** The ink starts at x = 70–71 for every time tested, including "10", "11" and "21". | `sheet_rest_f3.png` |
| 9 | SPLIT data column pops in; weak states | **Fixed.** The rows fade in. Charging shows ⚡50 / ⚡100. 4% battery shows "BAT 4" in red with the label raised. In AOD, BAT sits on its own row. | `extremes/x3_*.png` |
| 10 | MATRIX 68-cell ring at uneven speed | **Fixed.** The ring has 60 heads and advances at 1 Hz, on frames 29 and 59. | `seconds/s1.png` |
| 11 | MATRIX sparkles inside the digit boxes | **Fixed.** Sparkles now land only off-glyph, e.g. at (356,127) and (92,356). | `seconds/s1_*.png` |
| 12 | MATRIX minute change out of sync | **Fixed.** The drain happens at second 0. | `rollover/m1.png` |
| 13 | LIFE now-dot disappears at the minute change | **Fixed.** The red dot stays through the minute change and grows from 60 → 256 px at (280,188). | `aodwake/am4.png`, `rollover/m4.png` |
| 14 | LIFE hour drain collapses | **Fixed.** At 11:00 the 59th dot whitens. The grid then drains from the end, about 6 dots per frame (2,330 px/frame, frames 30–39), the time rolls mid-drain, and the red now-dot grows in place at index 0 over frames 40–49. | `life/lh.png`, `life/lh_drain_crop.png` |
| 15 | TAPE hub snap and stutter | **Fixed.** Each step is 3.2 / 10.2 / 11.2 / 5.5° over 4 frames. There is no snap at the end of the build-in. | `tools/hub2.py`, `rollover/m7.png` |
| 16 | TAPE reels encode nothing | **Fixed.** The pack is the day fraction, about 8 min per dot. The new midnight swap is #8 below. | `rest/f7_*.png` |
| 17 | TAPE lamp hard-blinks; date off centre | **Fixed.** The lamp has a blink envelope, and the lamp + date pair is centred. | `sheet_rest_f7.png` |
| 18 | INVERTED magenta ghosts | **Fixed.** Ghosts are now 0xEF7D on white and 0x18C3 on black. | `rest/f6_1009.png` |
| 19 | INVERTED page dots inverted | **Fixed.** Ink dots on white. | `pager/p6.png` |
| 20 | SEGMENT hero small; red overflow rings | **Fixed.** The cap height is about 74 px, and only the steps ring turns red. | `rest/f5_2130.png`, `h12/h12_f5_2105.png` |
| 21 | SEGMENT live gains take 4 s | **Fixed** (per code: step = ad/12, so any gain lands in ≤ 0.4 s). I didn't re-film it. | `face_segment.c` |
| 22 | No pressed state on complications | **Mostly fixed.** SPLIT rows, the SEGMENT values row and the INVERTED steps label show a pressed rect, and the iris opens from the touch point. Two gaps remain (#12 and #13 below). | `press_rects.png`, `taps/t3hr.png`, `taps/t5val.png`, `taps/t6.png` |
| 23 | MATRIX page dots overlap the grid | **Fixed.** The capsule is drawn from the first frame of the drag. | `pager/p1.png` |
| 24 | GLYPH hour hand tint | **Fixed.** Both hands are C_WHITE. | `rest/f2_1009.png` |
| 25 | Second dot motion uneven | **Fixed.** GLYPH steps 0.200° per frame (sd 0.008°); INVERTED has sd 0.022°. | `seconds/s2.png`, `seconds/s6.png`, `tools/redpath.py` |
| 26 | LIFE breath in visible steps | **Partly.** It is smoother, but asymmetric: the dot holds 15 frames at its minimum and 9 at its maximum, with 1-frame holds at frames 15 and 45 (#17 below). | `seconds/s4.png` |
| 27 | SEGMENT values row left-heavy | **Partly.** The column centres are now −89 / 0 / +79, but the value group's ink centre is x = 220 (13 px left of centre) while the labels are at 227.5 (#18 below). | `rest/f5_1009.png` |
| 28 | SPLIT data column 5.5 px low | **Fixed.** The column centre is at 233.5. | `rest/f3_1009.png` |
| 29 | DOT labels hug the bezel | **Fixed.** Y_DATE is 50 and Y_STAT is 428. | `rest/f0_1009.png` |
| 30 | 12 h zero-pads | **Partly.** DOT, MATRIX, SPLIT, INVERTED and TAPE blank the leading digit. LIFE and SEGMENT still show "09:41" (#9 below). | `sheet_h12.png` |
| 31 | Not exercised (bpm, 0 steps) | **Partly.** 0 steps at 04:20 renders correctly on all faces. `set bpm` still doesn't reach `svc_heart()`, so SPLIT's red HR state is still unseen. | `rest/f*_0420.png` |

## Issues (most severe first)

1. **[P1] Face picker opened from Settings: the face fades up to full, then hard-cuts to Settings.**
   Settings → Watch face pushes the picker. After you choose MATRIX:
   - Frames 3–7: the pill fades and the face brightens from the 18/32 ink dim to full. It reads exactly like
     "going home".
   - Frame 8: a hard cut (TX_NONE) to the Settings list.
   That's a full-panel swap from a bright face to a list in one frame, after a motion that promised the
   opposite. The cause is that `finish()` always calls `nos_pop_with(TX_NONE)`. That is right when the picker
   sits on home, where the face underneath is already identical, but wrong here.
   Evidence: `fromsettings/fs_choose.png` frames 3–8, `fromsettings/fs_after.png`.
   Fix:
   - In `finish()`, use TX_NONE only when the screen underneath is home.
   - Otherwise, skip the brighten and pop with TX_DISSOLVE (the same transition as the push), or iris back to
     the "Watch face" row.
   - Better: when entered from Settings, keep the face dimmed during the 0.17 s confirm, then dissolve. The row
     value ("MATRIX") already confirms the change.

2. **[P1] SEGMENT, wake from AOD: 70% of the final light arrives in the last frame.**
   - Frames 0–3: the AOD layout cross-fades as designed. That is the AOD step-dot ring, the outlined time and
     "2871 STEPS" fading out.
   - Frame 4 (k = 1): all three sprite rings, the three-column values with labels, and the full-white time
     appear at once. That is 70.1% of the final luma in one frame.
   - The unread dot also switches from white to red in that frame.
   A wake at a minute change shows the same thing (69.5%). This is the largest single-frame light change in
   the faces set and is easy to see at the wrist.
   Evidence: `aodwake/w5.png` frames 3→4, `aodwake/am5.png`.
   Fix:
   - Start the rings at k = 0.5. Draw the sprite rings with `ui_dim(col, (k − 0.5) / 0.5)` quantised to the
     wake steps, so they arrive over 2 frames. Or grow them radially from the AOD ring radius.
   - Cross-fade "2871 STEPS" into the values row: values at `ui_dim(C_PRI, k)` from k = 0.5, and the label
     out by 1 − k.
   - Mix the unread dot colour by k (`ui_mix(C_PRI, C_RED, k)`).
   - Target: no wake frame adds more than about 30% of the final luma.

3. **[P2] GLYPH, wake from AOD: the date, second dot and unread dot all pop in the last frame.**
   At k = 1 (frame 3 → 4), three things happen in the same frame:
   - "SUN 11" appears at full C_SEC.
   - The red second dot appears at r = 6.
   - The unread dot jumps about 250 px, from under the 12 tick (233, ≈52) to the left of the date slot
     (≈ (64, 233) at 10:14, when the date sits at 9 o'clock).
   The hands and tick ring morph smoothly in the frames before.
   Evidence: `glyph_wake_pop.png` (`aodwake/w2_003` vs `w2_004`), `aodwake/w2.png`.
   Fix:
   - Draw the date from k ≥ 0.5 at `ui_dim(C_SEC, (k − 0.5) / 0.5)`.
   - Grow the second dot over the last 2 steps: r = 6 · (k − 0.5) / 0.5.
   - Place the AOD and wake unread dot at the same slot position (call `date_slot` in AOD too), so it never
     moves. Or keep it under the 12 tick in both states and drop the "leads the date" placement.

4. **[P2] MATRIX, wake from AOD: the seconds ring and its head pop in at full.**
   The digits morph, then at k = 1 the lit ring cells (up to 60) and the bright head appear in one frame. That
   is 34% of the final luma.
   Evidence: `aodwake/w1.png` frames 3→4.
   Fix: grow the ring cells with k from 0.5, e.g. cell size `full · (k − 0.5) / 0.5` with colour mixed from
   C_BG. Or light the ring in a single 4-frame sweep from 12 o'clock to the current second (≤ 15 cells per
   frame), like the build-in ripple but faster.

5. **[P2] Face picker: swiping to a face last seen minutes ago replays a stale morph.**
   The picker ticks only the visible face and its neighbour, so a face that has been off-glass carries old
   state:
   - INVERTED, last seen at 10:09 and revisited at 10:31: its digits dot-morph 10:09 → 10:31 while it slides
     in (frames 7–9).
   - SPLIT, last seen at 11:40 and revisited at 12:05: it runs an odometer roll 11:40 → 12:05 (frame 12).
   - LIFE (`last_m`) and any face using `fk_morph` would do the same.
   Rolling to the right time for no reason reads as a glitch. This is the typical picker path: you change
   faces and come back later.
   Evidence: `stale_inv_crop.png` (`stale/st_inv_007..009`), `stale_split.png` (`stale/st_split_012`),
   script `scripts/stale.txt`.
   Fix:
   - In face_kit, snap instead of morphing when the face hasn't been ticked for a while: `fk_morph_tick` sets
     the morph to done when `c->t − last_t > 0.5 s`.
   - SPLIT re-inits its roll state, and LIFE resets `last_m`, on the same gap.
   - Or, in the picker's `f_enter`, warm-tick every face once with the morph forced to done.

6. **[P2] INVERTED, waking at a minute change: the digits hard-cut during the disc opening.**
   Woken at 10:15:59, the digits switch 10:15 → 10:16 in one frame (frames 5 → 6) inside the growing disc. The
   opening path draws `dot_text_a(S.tm.to)` with no morph, so every other face's minute morph is skipped here.
   In the same run, the unread dot pops in at full when the opening ends (frame 16, P3).
   Evidence: `aodwake/am6.png` frames 5–6 and 16.
   Fix:
   - Draw the digits with `dot_text_morph` during the opening too. The disc clip already bounds them.
   - Fade the unread dot with the opening's last 2 frames: `ui_mix(C_BG, ink, q)`.

7. **[P2] GLYPH: the date jumps between slots in one frame.**
   At 10:12 the minute click moves the minute hand into the 3 o'clock window. In that frame the date and unread
   dot leave 3 o'clock and appear at 9 o'clock, about 240 px away. Because the slot is evaluated from the
   animated angle, it flips mid-click. Every slot change behaves like this: 3 → 9 → 6 → 12 and back.
   Evidence: `glyph_slot.png` (`glyph/g_slot_034` → `035`), script `scripts/glyphslot.txt`.
   Fix:
   - Keep `S.slot` and change it only when the minute ticks (from `dt.minute`, not the animated angle).
   - Cross-fade old → new over 4 steps (0.2 s), using `ui_dim` on both labels.
   - Add hysteresis: leave a slot at 18° but re-enter it only at 24°, so a hand resting near the boundary
     can't flicker it.

8. **[P2] TAPE, midnight: both reels swap their packs in one frame.**
   At 00:00 the supply reel jumps from empty to full and the take-up from full to empty: 8,684 px change in
   frame 20, 47.5% of the reel luma. In the same frame:
   - "SUN 11" hard-cuts to "MON 12".
   - The timecode still reads 23:59:59; its morph runs over frames 21–24.
   This is the only moment in the day when the face's metaphor (tape running) is visible, and it is a
   cut.
   Evidence: `tape_midnight.png` (`rollover/m7_019`, `020`, `024`), `rollover/m7.png`.
   Fix:
   - Play a 0.5 s rewind: the take-up drains dot by dot into the supply (about 15 frames, outer ring first)
     while both hubs spin at 4× the step rate.
   - Start it on the same frame as the timecode morph.
   - Cross-fade the date over 4 steps.

9. **[P2] 12 h mode: LIFE and SEGMENT zero-pad, TAPE's timecode is off centre, DOT and MATRIX hours go
   right.**
   With "24-hour clock" off (Settings, 4 drags down, toggle at (364,344)):
   - **LIFE and SEGMENT** show "09:41" and "09:05", while the other six faces show "9".
   - **TAPE** renders " 9:41:13" with the blank digit kept, so the ink centre is at x = 250.5, 17.5 px right of
     centre.
   - **DOT and MATRIX** keep the lone "9" in the right-hand column, so DOT's hour block centre is 283.5
     (+50 px). This is defensible as a tabular flip-clock, but next to centred minutes it reads as a missing
     digit.
   - **SPLIT** (left-aligned "9") and **INVERTED** (ghost cell) are correct.
   Evidence: `sheet_h12.png` (`h12/h12_f*_0941.png`, `h12/h12_f*_2105.png`), `h12_set.png`.
   Fix:
   - LIFE and SEGMENT build the time from `fk_hh`/`fk_mm` and trim the leading blank.
   - TAPE measures and centres the trimmed string.
   - Optional: DOT and MATRIX centre a single hour digit on the block axis (x = 233) and keep minutes
     two-column.

10. **[P3] All `fk_wake` faces: the wake rhythm is 1-2-1-1.** `k = ceil(((t − t0 + 1/30) / 0.2) · 4) / 4` yields
    0.25, 0.5, 0.5, 0.75, 1.0, so frame 1 → 2 changes 0 px on every face. It reads as a hitch.
    Evidence: `aodwake/w0..7.png` frames 1–2 (identical).
    Fix: count frames instead of time, `k = min(1, (frames_since_wake + 1) / 4)`, or use WAKE_S = 4/30 s with
    the same formula.

11. **[P3] SPLIT, wake: the unread dot goes grey → white → red.** It is C_PRI through the morph and switches to
    C_RED at k = 1. Evidence: `aodwake/w3.png`. Fix: pass `ui_mix(C_PRI, C_RED, k)` (or the alarm colour) as
    the rest colour.

12. **[P3] SPLIT, complication pressed rect is a fixed width.** Every row's rect spans x 284–440. For "HR 68" the ink
    ends at x = 379, so the rect runs about 60 px past it and touches the bezel. Evidence: `press_rects.png`, `taps/t3hr.png`. Fix:
    `r[2] = vx + value_width + 16`, with the value width measured from the drawn string, and r[0] = X_COL − 16.

13. **[P3] SEGMENT, the ring tap has no pressed state.** Taps at a distance ≥ 160 from centre open Activity,
    but `face_segment_hit` returns an all-zero rect, so nothing lights on IN_DOWN. Evidence: `taps/t5ring.png`.
    Fix: return the rings' bounding square (CX ± 222) with a circular mask. Or simpler: brighten the three ring
    tracks from C_DIS to C_SEC while pressed.

14. **[P3] AOD: only SPLIT shows low battery.** At 4% in AOD, SPLIT keeps "BAT 4" on its row, and the other
    seven faces show nothing. Evidence: `extremes/x*_bat4_aod.png`. Fix: below 10%, draw a 6 px C_SEC battery
    glyph or "4%" at the face's `fk_status` position in AOD. This adds about 0.1% lit, so it stays in budget.

15. **[P3] LIFE: the unread dot sits close to the day ring's hour-0 dot.** The unread dot (white, 9 px, y = 52)
    and the ring's hour-0 dot (grey, 9 px, y ≈ 21) are 31 px apart on the same axis. At a glance they read as
    a pair. Evidence: `unread/u4_unread.png`. Fix: put the unread dot 14 px left of the date label, at the
    date's baseline (as GLYPH does), or move it to y = 64.

16. **[P3] All faces: the date hard-cuts at midnight.** Every face swaps "SUN 11" → "MON 12" in one frame
    while the time morphs. Evidence: `rollover/m0..7.png` (the midnight frame), `dot_midnight.png`. Fix: a shared
    `fk_label_xfade` (4 steps, out then in) for the date label.

17. **[P3] LIFE: the breath is asymmetric.** The dot holds 15 frames at its minimum but only 9 at its maximum,
    with 1-frame holds at frames 15 and 45. That makes the cycle feel like a stumble. Evidence:
    `seconds/s4.png`. Fix: drive it from `fk_ease` of a symmetric triangle over 2 s, quantised to 8 levels, so
    each level holds 3–4 frames on both slopes.

18. **[P3] SEGMENT: the values row is still 13 px left-heavy.** The ink centre of "2871 115 31" is x = 220,
    while the labels row is at 227.5. Evidence: `rest/f5_1009.png`. Fix: centre each value on its label
    centre rather than on a fixed column, or move the column centres to CX − 80 / CX + 6 / CX + 86.

19. **[P3] Face picker (shell team's in-flight moat, for reference).**
    - On INVERTED the ink dim turns the white disc into a 138-grey disc, the largest lit area in the picker.
      The moat then draws a heavy black halo around the pill, and the page dots over that grey have low
      contrast (`picker_pages.png`, `picker/k_on6.png`).
    - On DOT the pill (y ≈ 382–418) covers the bottom dot row of the minutes (y 380–392)
      (`picker/k_open_039.png` vs `rest/f0_1009.png`).
    Fix:
    - Dim INVERTED's disc toward 0x8C51 (the opening's mid-grey) rather than by ink ratio, and draw page dots
      as ink on it, as home does.
    - Lower the pill to a centre of y = 410 (it clears DOT's digits and still sits 12 px above the page dots at
      440), or let the moat feather only the pill rect + 6 px.

## What is already excellent (don't break it)

- **INVERTED's opening:** the disc grows from the AOD in 0.36 s with mid-grey digits, with no white flash and no
  ghost-on-black. It is the best wake in the set. Its second dot rides the hairline with sd 0.022°/frame.
- **GLYPH's second dot:** a 0.200°/frame glide with sd 0.008°. The tick-ring morph from AOD is lovely, the date
  is never under a hand, and both hands are true white.
- **SPLIT's typography:** a dead-straight left edge at x 70–71 for every time, and odometer rolls on the changed
  digits only. It is also correct in 12 h. Its data column is centred, and the charging / low-battery states
  are excellent (⚡50, red "BAT 4" with the label raised).
- **MATRIX:** the ripple build and drain are on the beat (second 0), the ring advances at 1 Hz with 60 heads,
  and sparkles stay off the glyphs.
- **TAPE's mechanics:** hub steps of 3.2/10.2/11.2/5.5° with a mechanical ease, no snap, and reels that now read
  as the day running.
- **LIFE's hour drain:** the 59th dot whitens, the grid drains from the end in 10 frames, the time rolls
  mid-drain, and the new now-dot grows in place.
- **SEGMENT:** it builds from the last fill shown (it reads as "what you walked since you last looked"), the
  red overflow is limited to the steps ring, and the hero is 74 px.
- **Unread and battery coverage:** all 8 faces carry the unread dot (6 px C_SEC in AOD) and battery/charging,
  and opening notifications clears it everywhere.
- **Complication presses:** the pressed rect appears on IN_DOWN, and the iris opens from the touch point.
- **Pager:** capsule page dots on full-bleed faces and ink dots on INVERTED, from the first frame of the drag.
- **Picker → home:** choosing a face from the long-press picker returns to home with no rebuild and no flash
  (`picker/k_choose.png`). The ink-dimmed faces are cheap and stay live.
- **Performance and AOD:**
  - `--damagecheck` passes on 3,920 frames (`dmg/`), and there are 0 late frames in every run.
  - AOD lit area per face: DOT 2.0–2.4%, MATRIX 1.3–1.5%, GLYPH 1.4%, SPLIT 2.0–2.4%, LIFE 1.2–1.3%,
    SEGMENT 2.3–2.6%, INVERTED 4.7–5.2%, TAPE 1.4–1.5%.
  - APL is ≤ 1.1% on every face, with no red in AOD.
