# healthtime — critic round 1

Scope: Activity, Heart rate, Breathe, Timer (+ custom picker, TIME'S UP), Stopwatch, Alarms
(list, editor, ringing), World clock (list, chooser, analog), widgets wg_activity / wg_heart /
wg_timer. Simulator `build/nos_sim_crit_healthtime.exe`; every script, still, contact sheet
and cost file is under `build/nos/critic/healthtime/` (scripts in `scripts/`). Evidence paths
below are relative to that folder.

## Verdict

This area is built on a strong engineering base. Across 13 scripts (about 9,000 frames) there
are **0 late frames**: the worst frame is 24.4 ms, and that is the bus cost of a full-screen
transition. **All 9 damage checks PASS.** Static screens really do go idle, including Breathe's
holds. The signature moves are present and mostly well tuned: ring dots fill, dot build-ins,
per-digit morphs, a 1:1 drum picker and a 1:1 slide-to-stop.

What keeps it from the bar is a set of finishing errors:
- a scrollbar that never fades in two apps,
- day chips cut off by the bezel,
- a heart trace that overshoots its box,
- a snooze state that contradicts itself,
- wrong world-clock times (no DST),
- about ten state changes that hard-cut in one frame where everything else in the OS moves,
- a family of green/magenta tints on dark greys.

None of these is hard to fix. Most are a few lines.

## Issues (most severe first)

1. **[P1] Activity + Heart — the scrollbar never fades out; it stays at 25% forever.**
   `vp_tick` decays `bar_k` exponentially (×0.883 per frame) and then rounds it to 1/16
   (`qz(...,16)`). At 0.25 a decay of 0.029 is smaller than half a step (0.031), so the value
   rounds back up to 0.25 every frame. The arc on the right bezel stays lit (thumb `#393839`)
   for as long as the app is open. In Activity this is exactly the crowding of the ring that
   `act_enter` tries to avoid. Evidence: `sb/act_after6s.png` (6 s after paging back),
   `heart/hr_rewrist_locked.png`, `heart/hr_nocontact.png`.
   Fix (both files): quantise in the direction of travel:
   `v->bar_k = target < v->bar_k ? floorf(v->bar_k*16)/16 : ceilf(v->bar_k*16)/16;`. Also, since
   the arc is `gfx_arc` (507 ns/px, 15% of Activity raster), drop to 0 when `bar_k < 0.2`.

2. **[P1] Alarm editor — the FRI and SUN chips are clipped by the bezel in the initial view.**
   `E_CHIP2_Y 420` and `E_CHIP_R 38` put the FRI chip centre (145,420) 206.7 px from the screen
   centre, so its outer edge sits at r = 244.7. That is 11.7 px past the glass. SUN is the
   mirror image, and SAT touches the glass. Evidence: `alarm/al_edit_new.png`,
   `alarm/z_chips.png` (2× crop).
   Fix: `E_CHIP_R 34`, horizontal pitch 80 (`chip_x`: `UI_CX + (k-1.5)*80` and
   `UI_CX + (k-5)*80`), `E_CHIP1_Y 328`, `E_CHIP2_Y 404`. FRI is then at r = 222.8 (10 px
   margin), and the hit radius of 44 still gives an 88 px target.

3. **[P1] Heart — while measuring, the trace overshoots its box and touches the heart.**
   The box is designed for yc = 238, h = 100 (y 188-288). The measured lit extent is y 158-302
   during frames 57-119 of `heart_meas/m_*.png`. The swollen heart's bottom reaches y = 157, so
   the spike and the heart touch (`heart_meas/m_060.png`). The cause: `H.lo/H.hi` follow the
   window with a 2.5/s low-pass, so every new, larger beat escapes the box before the range
   catches up. Fix in `playhead_tick`: widen instantly and narrow smoothly
   (`if (lo < H.lo) H.lo = lo; else H.lo += (lo-H.lo)*a;` and the same for `hi`), and clamp
   `trace_y` to `[yc-h/2, yc+h/2]`.

4. **[P1] Alarms — the snoozed state contradicts itself.** After SNOOZE, the list header says
   `NEXT IN 9 MIN` while every row's toggle is OFF (a one-shot alarm is switched off when it
   fires), and no row shows which alarm is snoozed. Evidence: `alarm_ring/ar_list_snoozed.png`.
   Fix: while `s_snooze_idx >= 0`, draw that row's day label as `ui_state` `SNOOZED 07:09` in
   C_PRI, keep its time white, and draw its toggle on. A tap on that toggle cancels the snooze.

5. **[P1] World clock — the times are wrong by 1 h in daylight-saving periods.** At the sim date
   (2026-10-11) the list shows London 04:39 (actually 05:39 BST), New York 23:39 SAT (actually
   00:39 SUN, EDT) and Sydney 14:39 (actually 15:39 AEDT). That is 3 of the 4 default cities.
   The fixed-offset table is documented, but a world clock that is wrong is a broken clock.
   Evidence: `wc/wc_list.png`, `wc/wc_bottom.png`.
   Fix: add a `dst` rule byte to `city`, with a ~30-line `dst_active(rule, utc)`:
   - EU: last Sun Mar 01:00 UTC to last Sun Oct 01:00 UTC (London, Paris).
   - US: 2nd Sun Mar to 1st Sun Nov at 02:00 local (Los Angeles, New York).
   - AU: 1st Sun Oct to 1st Sun Apr (Sydney).
   - None: all other cities.

6. **[P2] State changes that hard-cut in one frame.** The rest of the OS moves, but these pop:
   - Timer: setup to run and run to setup. The six pills vanish in one frame (f32→f33,
     `timer/tm_run_build.png`; f133→f134, `timer/tm_cancel.png`).
   - Timer pause: digits white→C_SEC, `1 MIN`→`[ PAUSED ]` and pause→play all swap in one
     frame (f70→f71, `timer/tm_pause.png`).
   - Heart: locked→no contact blanks the number, trace and labels in one frame (f361→f362,
     `heart/hr_offwrist.png`). No contact→measuring swaps the illustration for the trace in one
     frame (`heart/hr_rewrist.png` f438).
   - Heart lock: `[ MEASURING ]` and the quality dots disappear in one frame (f123→f124,
     `heart_lock2/L_000-001`, lit-pixel bbox 147,325-317,378 → none).
   - Breathe: AGAIN removes the whole summary in one frame (f1947→f1948,
     `breathe2/br_again.png`).
   - wg_timer: the quick circles vanish in one frame when one is tapped (f210→f211,
     `widgets/wg_timer_tapq.png`).
   - Stopwatch RESET: the lap rows vanish in one frame (f459→f460, `sw/sw_resetfilm.png`).

   Fix, one recipe for all of these: the outgoing content departs over 150 ms
   (`EASE_ACCEL`, alpha quantised to 6 levels, or a reverse `dot_text_anim`), then the incoming
   content builds. For pause, morph the label and fade the digit colour over 150 ms. For
   `[ MEASURING ]`, the fade already exists (`km`) but `u` jumps too fast; drive `km` from
   `lock_t / 0.2` instead.

7. **[P2] Custom picker and alarm drums — the rubber band is far too loose.** Overdragging the
   minutes drum 220 px past 00 moves `00` 86 px out of its selection band and into the `MIN`
   label, leaving the band empty for about 8 frames (`timer/tm_drum_rubber.png` f301-f309; a
   flick does the same, `timer/tm_drum_flick.png` f376-f381). Pickers should barely move past
   their ends. Fix in `tdrum_input`: `rubber_band(raw - lo, d->row * 0.4f)` (and the same for
   `hi`), which caps the excursion at about 25 px.

8. **[P2] Opening the timer from its widget rebuilds digits that are already on screen.** The
   widget shows `04:57` at DIG_TOP 178. The app opens with the same digits in the same place,
   but dissolves them into a random-dot sparkle for 4 frames (`wg_timer_open/open.png`
   f261-f264). It also moves the total label from below the digits (widget y 300) to above them
   (app y 150) and swaps the ring from r 182 to r 211. Fix: when `T.tm.state != 0` at
   `timer_enter`, draw the digits with `build = 1` (the controls and ring can still build). Put
   the widget's total label at y 150 like the app. Drop the widget's `TIMER` label, because the
   ring already says what it is.

9. **[P2] Stopwatch — the second hand runs 1 rev/s, so it strobes.** At r 211 that is 44 px per
   frame. The 3-dot trail is spaced 16 px, so it cannot bridge the steps, and the trail dots are
   muddy dark reds (`ui_dim(C_RED,0.55/0.3)`) (`sw/sw_start.png`). This is swoosh, not click.
   Fix: either (a) a 60 s sweep (1.2°/frame) with no trail, or (b) keep 1 Hz but step 36° per
   tenth, in sync with the tenths digit, with a single dot and no trail.

10. **[P2] Stopwatch "stopped" looks identical to "running".** Stopped digits change from
    C_WHITE to C_PRI (#FFFFFF → #E8E8E8), which you cannot see (`sw/sw_stopped.png`). The
    timer's paused state uses C_SEC plus `[ PAUSED ]`. Fix: stopped → C_SEC digits, plus
    `ui_state("STOPPED")` at y 150 when there are no laps. That makes it consistent with the
    timer.

11. **[P2] Green/magenta casts on dark greys.** `ui_dim` (and the per-file `gdim`, at low
    levels) rounds the 5-bit and 6-bit channels separately:
    - Pill fade-in alternates #101410 (green) and #181418 (magenta) frame to frame
      (`br_entry/e_005-007`).
    - Faded alarm rows and toggles are #313831 (`alarm/al_list_scrolled.png`).
    - Timer control circles build in at #000400 / #080C08 (`wg_timer_open/open_008-010`).
    - C_GHOST itself is #181C18 (the unlit hero-digit cells, `activity/act_p0.png` 160,200).

    Fix: one neutral dimmer in `ui.c`, `l5 = lroundf(lum*k*31); return l5<<11 | (l5*2)<<5 | l5;`
    used for every grey. Change C_GHOST to 0x18C3 (#181818).

12. **[P2] Activity / Heart — the incoming page is blank for the first half of a drag.** Pages
    1-2 start building only when `|pos-i| < 0.55`, so the user drags an empty black page up
    (`activity/act_drag_p1.png` f45-f48: page 1 area empty while 45% of it is on screen). Fix:
    start the build at `< 0.95` (first pixel of reveal), so the stagger finishes as the page
    lands. The same applies to `hist_started` in heart (`> PAGE_H*0.05f`).

13. **[P2] Heart widget with the module absent.** It still says `TAP TO MEASURE` and shows the
    big BPM. Tapping it opens "Module not connected" (`misc/wg_heart_absent.png`). Fix: if
    `!(hal_ext_present() & EXT_HR)`, replace the bottom label with `MODULE NOT CONNECTED`
    (C_DIS) and draw the number in C_DIS. Keep the history if any exists.

14. **[P2] Heart — the signal-quality bar reads as page dots.** It is 5 unlabelled grey dots
    centred at y 376 (measuring) and y 404 (locked), pitch 12-14, d 4.5-5. Its geometry is
    nearly identical to the home pager's dots at y 439 (`heart/hr_measuring.png`,
    `heart/hr_locked3.png`). Fix: label it (`SIGNAL` ui_label left of it, bar right-aligned in
    one row), or render it as a `ui_segments` 5-segment bar 60×6 so it reads as a meter.

15. **[P2] Heart history — mislabelled and redundant.** The page title says `TODAY`, but the
    chart is a rolling 24 h (axis `12 18 00 06 NOW`). `RESTING` always equals `MIN` (56 / 56),
    because svc computes `resting` as the minimum of the history (`heart/hr_history.png`).
    Fix: title `LAST 24 H`. Show `AVG` instead of `RESTING`, or compute resting as the mean of
    the lowest 10% of samples taken while still.

16. **[P2] Activity hourly / week charts — specks.**
    - At rest, 1-px bright specks appear inside ghost dots just above column tops. This is the
      rounding remainder of `v*255` drawn as a sub-pixel dot (`activity2/z_over_chart.png`,
      e.g. 187,244 = #838183 inside a #181C18 ghost). Fix: give `dot_columns` whole-dot
      values (`q = k*255/rows` with `k` an integer), or skip a partial top dot below 0.5.
    - The week goal line is drawn over the bars, so dark specks sit inside lit dots
      (`activity/z_p2_goal.png`). Fix: draw the goal line before the columns, and snap `gy` to
      a row gap (`y0 + k*pitch - pitch/2`).

17. **[P2] Activity entry — the label gives away the count.** `35% OF 8000` is shown at full
    value from frame 2, while the number still reads 0 and then counts up
    (`activity/act_entry.png` f4-f13; also `activity3/act_over_entry.png`, `135%` over `0`).
    Fix: derive the percent from the counter's current value, or fade the label in when the
    counter settles.

18. **[P2] Breathe summary — the hero sets a word in dot matrix.** `1 MIN` is drawn in
    DOT_FONT_TALL with a gap of about 60 px between `1` and `MIN` (`breathe2/br_summary.png`).
    Words belong in the grotesk or in labels. Fix: dot hero `1` (pitch 9), with ui_label `MIN`
    below it at y 252, matching `72 / BPM`.

19. **[P2] Timer total label reads `0 MIN 4 S`** (`timer2/tm_custom_start.png`). Fix: in
    `tm_total_label`, use `%u SEC` when under 60 s and `%u MIN %u SEC` otherwise. That matches
    the picker's `MIN` / `SEC`.

20. **[P2] World-clock chooser — text collision.** `NEW DELHI` ends at x 211 and `UTC+5:30`
    starts at x 221 (`wce/e2.png`). Fix: drop the `UTC` prefix (`+5:30`, `-10`, `±0`), which
    also unclutters every other row.

21. **[P2] World-clock analog view — crowded bottom line.** `13:39 +3:30` at y 384-400 sits
    between the 5 and 7 o'clock major ticks at (135,403) and (331,403). The text starts 23 px
    from a tick (`wc/wc_detail.png`). Fix: move the line to y 340, inside the dial, or drop the
    digital time and keep only `+3:30`.

22. **[P2] Alarm list is not sorted.** The rows show 09:00, 07:03, 07:00 in creation order
    (`alarm/al_list3.png`). Fix: sort by `h*60+m` after edits and adds (keep the editor's index
    in sync).

23. **[P2] Cramped vertical rhythm.**
    - Alarm ring: SNOOZE pill bottom y 306, slider top y 318, a 12 px gap
      (`alarm_ring/ar_ring.png`). Use 24 px: `R_SNZ_Y 250`, `R_SL_Y 362`.
    - Alarm editor: `IN 20H 51M` (top ≈ 264) sits 6 px under the drum neighbours `08/01`
      (bottom ≈ 258) (`alarm/al_edit_new.png`). With the chip change in #2, move `E_INFO_Y` to
      284.
    - Stopwatch: the third lap row (baseline 290) is 16 px above the buttons (top 306)
      (`sw/sw_lap5.png`). Use `LIST_Y1 290` (2.5 rows), or `BTN_Y 358`.

24. **[P2] Heart — the screen's one red element is a faint outline.** The locked heart is
    pitch 3.0 with 2.4 px dots, about 33×28 px. The dots render as 2×2 squares, so the round-dot
    signature is lost (`heart_lock/z_swell.png`). Fix: pitch 4.0, dot 3.2 (about 44 px wide).
    Better, a filled dot heart when locked, so the live indicator carries weight.

25. **[P3] The heart colour tweens white → pink (#EEB6BD) → red on lock**
    (`heart_lock/L_032-035`). That is a translucent-looking blend. Switch hard to red on the
    frame the BPM build starts, or rebuild the icon in red with `dot_icon_anim`.

26. **[P3] The heart trace no longer reads as dots.** Dots are spaced at 0.8×d, so the trace
    is a solid polyline. A 3-level grey fade at the left edge (x 58-128) reads as a gradient
    (`heart/hr_measuring.png`). Try spacing 1.25×d and no fade: clip at the left edge instead.

27. **[P3] Timer `+1` button** (5×7 `+1`) is ambiguous next to TIME'S UP's `+1 MIN`. Use
    `+1 MIN` as a ui_label under the circle, or `+1` with `MIN` in micro type inside it.

28. **[P3] wg_timer quick start.**
    - No pressed state on the 3 circles; the widget API gives no IN_DOWN, so add a 100 ms
      fill flash on tap.
    - `TAP TO START` is false outside the circles (that opens the app).
    - The toast `5 MIN TIMER STARTED` overlaps the ring's bottom arc and repeats what the
      widget now shows (`widgets/wg_timer_tapq.png`).

29. **[P3] Breathe intro.**
    - `START` vanishes in 1 frame at tap (f43).
    - There are 9 idle frames (≈0.3 s) of a static contracted field before `Breathe in` fades
      in (`breathe2/br_intro.png` f67-f75). Start the inhale text fade at `T_INTRO - 0.2`.

30. **[P3] TIME'S UP / alarm pulse.** The ring brightness is quantised to 6 levels, so it steps
    at about 7.5 fps (a full 21 ms repaint every 4th frame, `timer2/tm_ring_in.png`). Use 12
    levels (spec cap 16). Frames stay well under budget.

31. **[P3] Alarm toggle.** The row's time digits dim in 1 frame while the knob takes 5 frames
    (`alarm/al_toggle.png` f380→f381). Drive the digit colour from `A.tog[i].x`.

32. **[P3] Primary pills (ADD CITIES, ADD ALARM) build in by dimming white,** so for about 6
    frames they show the pressed colour (C_SEC grey) (`wce/wce_back.png` f391-f396). Build them
    by scale or opacity steps from C_SURF2 instead.

33. **[P3] Small inconsistencies.**
    - Activity page 1 has no title, while page 2 has `THIS WEEK` and Heart page 2 has `TODAY`.
    - Activity page-1 zeros (`0.0 / 0 / 0`) are white, while page 0 dims a zero hero to C_DIS
      (`activity2/act_steps0_p1.png`).
    - Stopwatch lap labels' left edges are 65/65/70 px, so they follow the circle at only a
      36 px row pitch and read as misaligned (`sw/sw_lap5.png`). Use one x for all visible
      rows.
    - World-clock `SUN` icon at pitch 2 reads as a dotted spinner ring.
    - The analog view's seconds dot pops in at the end of the build.

## What is already excellent (do not break)

- **Performance and correctness.** 0 late frames in every script. The p95 of frames that send
  pixels is ≤ 24.4 ms, and that is only full-screen transitions. Breathe sessions run at
  p95 14 ms. All 9 damage checks PASS. Static screens go idle: Breathe holds, timer between
  ticks, alarm list, paused timer.
- **Breathe.** The field reads as organic: rings lag outward and twist as they open. Holds are
  truly still. The outro collapses the field to a point before the summary builds. The START
  disc and duration pills have proper pressed states. The session HR average lands in the
  summary.
- **Drum picker.** 1:1 tracking, ticks per detent, glide-to-detent on fling, tap-a-neighbour
  stepping, START disabled at 00:00.
- **Slide to stop.** Knob tracks 1:1, the hint fades with progress, a partial release springs
  back without overshoot, the commit has haptic success, and the button snoozes.
- **Timer run view.** The ring drains with a partially shrinking end dot. Per-digit morphs take
  about 4 frames. The last 10 s get a red tip and dimmed leading zeros. Play is optically
  offset by 2 px.
- **Activity ring.** The segment-by-segment fill with count-up morphs, and the red overflow lap
  painting over the first lap (`activity3/act_over_entry.png`), are the best moments in the
  area. The widget builds while the page slides in.
- **Heart lock.** The trace descends and narrows, the BPM assembles from the centre, the heart
  turns red, and BPM changes morph (`heart_lock/morph.png`). The no-contact illustration (watch
  settling onto the wrist) is a good designed state.
- **Empty states.** Alarms and World clock each have a dot icon, one line and one primary
  action (`alarm/al_empty.png`, `wce/wc_empty.png`). Heart absent explains what the module
  adds.
