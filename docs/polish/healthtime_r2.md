# healthtime — critic round 2

Scope: Activity (3 pages, ring), Heart rate (absent, no contact, measuring, locked, history),
Breathe (start, session, summary), Timer (presets, custom picker, running, paused, TIME'S UP),
Stopwatch (running, laps, stopped, reset), Alarms (list, editor, ringing, slide to stop, snooze,
snooze cancel), World clock (list, chooser, analog view), and the widgets wg_activity,
wg_heart and wg_timer. I also checked every screen against the §8 standards in
docs/NOS_DESIGN.md.

Sim: `build/nos_sim_crit_healthtime.exe`, rebuilt at 5e1bd36 plus the time team's working tree
(snooze API, `gfx_fast_sin`). I re-ran the time scripts after the time team's last edit.

Evidence is under `A:\makerspace\smartwatch2.0\build\nos\critic\healthtime\r2\`, one folder per
script, and paths below are relative to that folder. Scripts are in
`build/nos/critic/healthtime/scripts/`: all round-1 scripts plus 14 new ones in `scripts/r2/`
(`reveal`, `reveal_time`, `sbfade`, `sbfade2`, `press`, `press2`, `sumpress`, `timer_r2`,
`timer_tip`, `sw_r2`, `ring_r2`, `edit_r2`, `wc_full`, `wcbar`). Frame numbers are the `fNNN`
labels on the contact sheets, and `_NNN.png` files are single frames. Nothing ran on the board
(COM3 untouched).

## Verdict

The round-1 list landed almost completely:
- 31 of 33 issues are fixed and 2 are partly fixed (#6 hard cuts, #8 widget to timer). No
  fix regressed the thing it fixed.
- The heart screen is now the best in the area: a trace that stays in its box, a labelled
  SIGNAL meter, a crossfaded off-wrist state, a real 24 h history page and a correct
  module-absent screen.
- Snooze is now coherent from end to end. World-clock times are right under DST.
- The stopwatch reads well: a 60 s sweep, grey when paused, lap rows aligned on one x.

What keeps the area from the bar is a smaller, sharper set of problems:
- **P1:** Heart and Activity throw away their state when revealed from behind a cover. Heart
  drops a locked reading back to MEASURING in one frame, and Activity jumps from the week page
  to page 0. Every time screen keeps its state in the same test.
- **P1:** this area's toast strings (SNOOZED FOR 9 MIN, UP TO 4 CITIES, ALARM LIST IS FULL) are
  wider than the glass at the new top position. On snooze the brackets fall off the pill.
- **P2:** pressed states still last 2 frames, mostly in a grey the engine itself calls
  invisible.
- **P2:** a few one-frame cuts remain (day chips, DELETE arming, stopwatch RESET), and the timer
  view departure is compressed into 2 frames.
- **P2:** the week's DAILY AVG includes today's partial day.
- **P2:** the longer slide-to-stop track now nearly touches the pulsing bezel ring.

Performance is clean: 0 late frames in 37 runs (24,816 frames), and the worst frame is 24.4 ms,
the bus cost of a full-screen transition. All 12 damage checks PASS: `dc_activity`, `dc_heart`,
`dc_breathe2`, `dc_widgets`, `dc_sw`, `dc_swr2`, `dc_alarm`, `dc_edit`, `dc_ring`, `dc_wc`,
`dc_timer`, `dc_timer2`.

## Round-1 verification

| # | Round-1 issue | Status | Evidence (r2/) |
|---|---|---|---|
| 1 | P1 scrollbar never fades (Activity, Heart) | **Fixed.** Linear ramp, hidden below 0.2; the arc is gone 6 s after paging. The fade steps are uneven (new #17). | `sb/act_after6s.png`, `sbfade2/sb_015..024.png` |
| 2 | P1 FRI/SUN chips clipped by the bezel | **Fixed.** r 34, pitch 80, rows at y 328/404. FRI's outer edge is at r 222.8, so there is a 10 px margin. | `alarm/al_edit_new.png` |
| 3 | P1 heart trace overshoots its box | **Fixed.** The range widens instantly and the trace is clamped. The lit trace spans y 185-290 and the heart's bottom is at y 152. | `heart_meas/m_*.png` |
| 4 | P1 snoozed state contradicts itself | **Fixed.** The row reads `SNOOZED 07:09` in white with the toggle on, and the header says `NEXT IN 9 MIN`. Tapping the toggle cancels: the label and header crossfade over f362-f368. | `ring_r2/list_snoozed.png`, `ring_r2/cx.png` |
| 5 | P1 world clock ignores DST | **Fixed.** At 04:39 UTC on 2026-10-11: London 05:39, New York 00:39, Sydney 15:39. The chooser shows +11 for Sydney and +13 for Auckland. | `wc/wc_list.png`, `wc/wc_bottom.png`, `wce/e2.png` |
| 6 | P2 one-frame state changes | **Partly fixed.** Fixed: timer pause, heart off-wrist and rewrist, `[ MEASURING ]` on lock, Breathe AGAIN (0.18 s build-out). Still short: timer setup↔run (3 dead frames, then a 2-frame dissolve, new #4); wg_timer quick circles (out in 2 frames, f214-f215); stopwatch RESET (completed laps fade, but the live row cuts, new #5). | `timer_r2/go.png` f31-f36, `timer_r2/cx.png` f91-f96, `widgets/wg_timer_tapq.png`, `sw_r2/rs_001-002.png` |
| 7 | P2 drum rubber band too loose | **Fixed.** `rubber_band(raw-lo, row*0.4)`; an overdrag stays inside the band. | `timer_r2/pkr.png` |
| 8 | P2 widget→timer rebuilds digits | **Partly fixed.** The digits are kept and the total label is at y 150 in both. The ring is r 158 vs r 211; the team declared this (the pager reserves the top and bottom centre) and I accept it. The identical `TIMER` title and `5 MIN` label still rebuild (new #24). | `wg_timer_open/open.png` f262-f269 |
| 9 | P2 stopwatch hand strobes | **Fixed.** 60 s sweep (4.4 px/frame) with no trail. New P3: it jumps to 12 o'clock on RESET (#5). | `sw/sw_start.png`, `sw/sw_lapfilm.png` |
| 10 | P2 stopped looks like running | **Fixed.** The digits step to C_SEC over 150 ms (f437-f443), with `[ PAUSED ]` above them when there are no laps. That matches the timer. | `sw/sw_stopfilm.png`, `sw/sw_stopped.png` |
| 11 | P2 green/magenta greys | **Fixed for tokens.** ui_mix has a neutral path and C_GHOST is 0x18C3. Anti-aliased edges still pick up casts (engine, new #23). | `br_entry/e_*.png`, `alarm/al_list_scrolled.png` |
| 12 | P2 incoming page blank for half a drag | **Fixed.** The build starts at `|pos-i| < 0.95`. | `activity/act_drag_p1.png` |
| 13 | P2 heart widget with module absent | **Fixed.** Shows `[ NO HR MODULE ]`. The naming differs from the absent screen (new #22). | `misc/wg_heart_absent.png` |
| 14 | P2 signal bar reads as page dots | **Fixed.** `SIGNAL` label plus a 5-segment 60×6 meter. | `heart/hr_measuring.png`, `heart/hr_locked3.png` |
| 15 | P2 history mislabelled / redundant | **Fixed.** `LAST 24 H`, MIN / AVG / MAX. | `heart/hr_history.png` |
| 16 | P2 chart specks | **Fixed.** Columns use whole dots; the goal line is drawn first and snapped to a row gap. | `activity/act_p1.png`, `activity/act_p2.png` |
| 17 | P2 entry label gives away the count | **Fixed.** The hero is a 0.30 s sparkle build; labels arrive after 0.6 × build. | `activity/act_entry.png` |
| 18 | P2 Breathe summary word in dot matrix | **Fixed.** Dot hero `1` at pitch 9, with `MIN` as a label. | `breathe2/br_summary.png` |
| 19 | P2 `0 MIN 4 S` | **Fixed.** Now `6 SEC`. | `timer_r2/tm_short.png` |
| 20 | P2 chooser collision | **Fixed.** Offsets read `+5:30` / `-10` / `+11`. | `wce/e2.png` |
| 21 | P2 analog bottom line crowded | **Fixed.** Only `+3:30` remains, 65 px clear of the 5 and 7 o'clock ticks. | `wc/wc_detail.png` |
| 22 | P2 alarm list unsorted | **Fixed.** 07:00, 07:03, 09:00. | `alarm/al_list3.png` |
| 23 | P2 cramped rhythm | **Fixed.** Alarm ring: SNOOZE to track is 26 px. Editor: `IN …` is 16 px under the drum neighbours. Stopwatch buttons at y 354 (declared, accepted) leave 20 px under the third, fading row. The new track geometry creates a different problem (new #8). | `alarm_ring/ar_ring.png`, `alarm/al_edit_new.png`, `sw/sw_lap5.png` |
| 24 | P2 heart red element faint | **Fixed.** Pitch 4.0, dot 3.2. | `heart_lock2/L.png` |
| 25 | P3 white→pink→red tween | **Fixed.** Hard switch to red on lock. | `heart_lock2/L.png` |
| 26 | P3 trace not dots | **Fixed.** | `heart/hr_trace.png` |
| 27 | P3 ambiguous `+1` | **Fixed.** `+1` with micro `MIN` inside the circle. | `timer2/tm_last10.png` |
| 28 | P3 wg_timer quick start | **Fixed.** 100 ms fill flash, `TAP TO START` removed, no toast. | `widgets/wg_timer_quick.png`, `widgets/wg_timer_tapq.png` f209-f213 |
| 29 | P3 Breathe intro | **Fixed.** START ink fades in 0.1 s; the inhale text starts before the hold ends. | `press/st_*.png`, `breathe2/br_intro.png` |
| 30 | P3 pulse quantised to 6 levels | **Fixed.** 12 levels (`time_timer.c:241`). | `timer2/tm_ring_pulse.png` |
| 31 | P3 alarm toggle digit cut | **Fixed.** The time dims with the knob over f381-f387, and the header crossfades. | `alarm/al_toggle.png` |
| 32 | P3 primary pills dim up from grey | **Fixed.** Scale steps at full white. The label still pops in after a blank pill (new #21). | `alarm/al_entry_empty.png` f10-f16 |
| 33 | P3 small inconsistencies | **Fixed** (all five): page 1 titled `TODAY`; page-1 zeros dimmed; lap rows on one x (70); SUN icon replaced by a day/night dot; the seconds dot builds mid-entry (f341) instead of popping at the end. | `activity2/act_steps0_p1.png`, `sw/sw_lap5.png`, `wc/wc_detail_in.png` |

## New issues (most severe first)

No P0.

1. **[P1] Heart and Activity reset when revealed from behind a cover.**
   - Heart, locked at 70 BPM: long-press for the power sheet, then pop. As the sheet drops, the
     locked screen is revealed (f243-f256). It then hard-cuts at f256→f257 to an empty
     `[ MEASURING ]` trace, and the lock rebuilds from zero.
   - Heart, history page: back from a cover lands on page 0 (`hr_hist_before.png` vs
     `hr_hist_after.png`).
   - Activity, week page: hard-cuts at f783→f784 from `THIS WEEK` to page 0, with the ring
     re-segmented.
   - Cause: `heart_enter` always resets the samples, `locked`, the mode and `vp_init`; only `H.t`
     looks at `nos_revealed()`. `act_enter` calls `vp_init(&A.vp, NPAGES)` before its
     `nos_revealed()` branch.
   - §8 says revealed screens don't rebuild. The time screens pass the same test: the stopwatch
     keeps laps and timing, the timer keeps running and World clock keeps its scroll
     (`reveal_time/sw_rev.png`, `tm_rev.png`, `wc_rev.png`).
   - Evidence: `reveal_b/hr_reveal.png`, `reveal_b/hr_hist_before.png`,
     `reveal_b/hr_hist_after.png`, `reveal_b/act_reveal.png` (also `reveal/`, from the first
     build).
   - Fix:
     - `heart_enter`: put `if (nos_revealed()) { H.present = hal_hr_get(&H.r); return; }` before
       any reset. The sensor is still on, because `leave` only runs on pop.
     - `act_enter`: skip `vp_init` and `hero_start` when revealed, keeping `A.vp` and the built
       hero and pages.

2. **[P1] Toast strings in this area are wider than the glass at the top.**
   - `ui_draw_toast` now draws at y 30-72 and caps the pill at 290 px. The glass is only
     259 px wide across that band (computed for a 42 px pill with r 21 at y 30-72).
   - Alarm ring SNOOZE → `[ SNOOZED FOR 9 MIN ]`: the pill spans x 88-377. Its top edge is
     sliced by the bezel (row y 31 lit only from x 117 to 348). The text is wider than the pill,
     so `[` and `]` sit on the rounded ends, half on black (x 88-99 and 366-377).
   - Chooser full → `[ UP TO 4 CITIES ]`: 290-wide pill with the same sliced corners.
   - `[ ALARM LIST IS FULL ]` is 22 characters, so it is wider still (by the same per-character
     width, about 303 px of text).
   - The shell critic reports the engine side. This is the string side.
   - Evidence: `ring_r2/so_010.png`, `ring_r2/z_toast.png`, `wc_full/toast.png`,
     `wc_full/z_toast.png`.
   - Fix, with strings of 13 characters or fewer, brackets included:
     - `time_alarm.c:705`: `nos_toast("Snoozed")`. The list row already says
       `SNOOZED 07:09`.
     - `time_alarm.c:386` and `time_worldclock.c:304`: `nos_toast("List full")`.
     - Engine (`ui.c:717`): cap `w` at 256, and drop tracking to 1 when `tw > w - 40`.

3. **[P2] Pressed states: 2 frames, and mostly C_SURF3.**
   - §8 asks for a pressed pill that shows for at least 3 frames, even on a quick tap. Every
     control below clears its pressed state on IN_UP, so a quick tap shows it for 2 frames.
   - Most of them press to C_SURF3 (#2A2A2A), which `ui_button` itself calls "invisible on the
     panel" (`ui.c:462`).
   - Breathe: START, the duration pills and AGAIN (`press/st_*`, `press2/pl_*`,
     `sumpress/ag_*`).
   - Timer: presets, ✕ and +1 (C_SURF3); the pause disc (C_SEC). Evidence: `timer_r2/go.png`
     f29-f30, `timer_r2/cx.png` f89-f90, `timer/tm_pause.png` f69-f70.
   - TIME'S UP: STOP and +1 MIN (`timer_r2/rg.png` f658-f659).
   - Stopwatch: LAP and RESET (C_SURF3); STOP and RESUME (C_SEC, for 2 frames). Evidence:
     `sw/sw_stopfilm.png` f437-f438, `sw_r2/rs.png` f215-f216.
   - Alarm editor: day chips (`edit_r2/ch.png` f68-f69) and DELETE (`edit_r2/arm.png`
     f152-f153).
   - Alarm ring: SNOOZE presses from #181818 to #292829 (`ring_r2/sp_000.png`).
   - The alarm list already holds rows correctly (`press_t0` / `unpress_at`, ≥ 100 ms), but its
     cards also press to C_SURF3.
   - Fix:
     - Timing: copy the alarm list's `unpress_at = max(now, press_t0 + 0.1)` into fit_breathe,
       time_timer (setup, run, picker, ring), time_stopwatch, the alarm editor and the alarm
       ring.
     - Colours: pills, discs and chips press to C_SEC with C_BG ink (the `ui_button` /
       `ui_icon_button` rule). Cards (alarm rows, world-clock rows) press to C_LINE2, as
       `sys_notifs.c:255` does.

4. **[P2] Timer setup↔run departure is compressed into 2 frames.**
   - `OUT_S 0.15` with `EASE_ACCEL`, fed into the staggered build. The six pills stay at full for
     3 frames, then dissolve in 2 (f31-f36). The same happens on cancel (f91-f96).
   - wg_timer's quick circles also go in 2 frames (f214-f215).
   - Evidence: `timer_r2/go.png`, `timer_r2/cx.png`, `widgets/wg_timer_tapq.png`.
   - Fix: make the departure linear (`T.out = 1 - t/OUT_S`), and draw the outgoing view at
     build 1 with one flat alpha `qz(T.out, 6)`, so all 5 frames carry a visible step. Keep the
     tapped pill in its pressed fill until the departure ends. Do the same in the widget.

5. **[P2] Remaining one-frame cuts on time controls.**
   - Alarm editor, day chip toggle: C_SURF3 to white, and the label C_PRI to black, in one frame
     (f69→f70, `edit_r2/ch.png`). Fix: a per-chip `on_k` that steps 0↔1 over 0.15 s; fill
     `ui_mix(C_SURF2, C_WHITE, qz(on_k,6))`, ink `ui_mix(C_PRI, C_BG, qz(on_k,6))`.
   - Alarm editor, DELETE arming: in one frame (f153→f154, `edit_r2/arm.png`) the width jumps
     196→284, the label swaps and 3 red dots appear. Fix:
     - Spring the width over 0.18 s.
     - Crossfade the label through the fill: old out, new in, 4 levels each.
     - Build the dots with `DOT_REVEAL_CENTER`.
   - Stopwatch RESET:
     - The completed laps fade, but the live row `LAP 03 00:02.1` (#B4B2B4) and its hairline
       vanish in one frame (`sw_r2/rs_001.png` → `rs_002.png`). Ghost mode skips row 0 with
       `else if (r == 0) continue;`.
     - In the same frame, the worst lap's time brightens from #626562 to #8B898B, because ghost
       rows drop the best/worst colours.
     - The red second dot jumps from the 13 s position (439,189) to 12 o'clock in one frame
       (`sw/sw_resetfilm.png` f459→f460).
     - Fix: snapshot `cur_lbl` / `cur_time` into the ghost and fade row 0 with the rest; keep
       each ghost row's colour; sweep the hand back to 0 over 0.3 s (or fade it out and in).

6. **[P2] Activity week: DAILY AVG includes today's partial day.**
   - 7836 at 05:00, when today is 0 and is skipped. 7122 at 10:09, which is exactly
     (6 × 7836 + 2842) / 7, today's 2842 steps included.
   - The average falls every morning and climbs through the day.
   - Evidence: `activity2/act_steps0_p2.png`, `activity/act_p2.png`.
   - Fix: average `week[0..5]` only. Today stays visible as its own column.

7. **[P2] Breathe summary: the last line is 14 px from the buttons.**
   - The AVG baseline is at y 314; the AGAIN/DONE pair's top is at y 328.
   - The lines above are 28-40 px apart, so the block reads as falling onto the buttons.
   - Evidence: `breathe2/br_summary.png`.
   - Fix: move the block up 16 px (hero top 116, `MIN` 230, `BREATHS` 270, `AVG` 298). That
     leaves 30 px above the pair and 46 px below the title baseline.

8. **[P2] Alarm ring: the slide track nearly touches the pulsing bezel ring.**
   - `R_SL_W 290` / `R_SL_Y 360` put the far points of the caps at r 204.8.
   - The 60-dot pulse ring is at r 211 with d 6-8, so its inner edge is at r 207-208. The gap
     is 2-4 px at both ends.
   - Evidence: `alarm_ring/ar_ring.png`, `alarm_ring/z_track_end.png`,
     `alarm_ring/z_track_start.png`.
   - Fix: `R_SL_W 260`, `R_SL_Y 352`, `R_SNZ_Y 240`. The caps' far points move to r 192, which
     clears the ring by 15 px at peak. SNOOZE then sits 28 px above the track and 26 px below
     the digits. The knob travel goes from 210 to 180 px.

9. **[P2] Activity and Heart: nothing says there is a page below.**
   - With the scrollbar fixed (#1) and the heart's dots turned into the SIGNAL meter (#14),
     neither app shows any sign at rest that pages 1-2 (Activity) or the history page (Heart)
     exist.
   - The scrollbar only appears while you drag.
   - Evidence: `activity/act_p0.png`, `heart/hr_locked3.png`.
   - Fix: when the entry build completes (not on reveal), flash the scrollbar once: hold
     `bar_k = 1` for 0.7 s, then the normal fade.

10. **[P3] Stopwatch first lap: for 1 frame the rising digits overlap the incoming rows.**
    - At f81 the digits' bottom dot row (y ≈ 212) sits across the `LAP 02` label (cap top
      ≈ 205) and its time (`sw_r2/z_lp_004.png`). By f82 they are 7 px clear.
    - Fix: start the list fade later, `k = qz(clamp(lay*2.5f - 1.5f, 0, 1), 8)`.

11. **[P3] Stopwatch lap list rests between rows.**
    - The window is 98 px with 25 px fades at each edge. After a drag it shows only 2 rows,
      with an empty 36 px band at the top: `LAP 04`'s centre sits on the window edge at y 194
      (`sw/sw_scrolled.png`).
    - Fix: snap the rest position to multiples of `ROW_H`, or shorten the fade to 12 px.

12. **[P3] Short custom timers: the last-10 s red tip blinks.**
    - On a 6 s timer the ring drains 10 dots/s. The partial end dot cycles 32 px → 17 px →
      gone every 3 frames, so the red tip hops along the bezel at 10 Hz (`timer_tip/tip_000-011`,
      bbox 186,436 → 208,440 → 230,441 → 252,440).
    - Fix: in the last 10 s, draw the tip as a full red dot at the exact fractional angle,
      with no partial sizing.

13. **[P3] Ring screens rise with a sheet grabber that sits on the alert dot.**
    - TX_RISE draws a grey 30×5 grabber at `top + 14` (`nos.c:873`). On the alarm ring it
      overlaps the red alert dot for f186-f189, then vanishes in one frame at f190
      (`ring_r2/z_top_strip.png`).
    - It also suggests a drag-down that `SCR_NOBACK` screens refuse.
    - Fix (engine): fade the grabber with the last 30 % of the rise, and skip it for
      `SCR_NOBACK` screens.

14. **[P3] Alarm ring entry: SNOOZE and the track fade up from black, and the label from grey.**
    - `ui_dim(C_SURF2,k)` and `ui_dim(C_PRI,k)` (`ring_r2/in.png` f180-f182) are against §8:
      buttons should be at full colour, with no grey fade-ups. The knob already does it right
      with scale steps.
    - Fix: scale SNOOZE and the track `0.72 + 0.28k` at full C_SURF2, with the label at full
      once `k ≥ 1`.

15. **[P3] Heart history `LAST` changes by text swap and shows while live.**
    - With a locked reading, `LAST 71 BPM` becomes `LAST 73 BPM` in one frame
      (`heart/hr_to_hist.png` f268). It is not "last" while the measurement is still running.
    - Fix: while locked, label it `NOW` and morph the number with the hero; show `LAST` only
      once the sensor is off.

16. **[P3] The Activity hero morph is not capped at 1 Hz.**
    - A new 0.28 s morph starts whenever steps change and the previous morph has settled. While
      walking, that is up to about 3.5 morphs/s; §8 allows about 1 Hz for live values.
    - Found by reading `fit_activity.c`.
    - Fix: `if (nos_t - A.morph_t0 >= 1.0f)` before starting a morph.

17. **[P3] Scrollbar fade-out steps unevenly and ends with a pop.**
    - Thumb levels per frame: #EE, #DE, #BD, #B4, #A4, #83, #73, #6A, #4A, #39, then gone. The
      steps are 9 to 33 levels apart, and the last level (57) disappears at once because of the
      `< 0.2` cutoff (`sbfade2/sb_015..024.png`).
    - Fix: 8 equal steps over 0.3 s, `qz(alpha, 8)`, drawn down to the last step (the arc cost
      only applies while visible).

18. **[P3] Breathe SUMOUT: DONE dims through its pressed grey.**
    - When AGAIN is tapped, DONE passes through C_SEC-like levels while it builds out
      (`sumpress/ag_002-004.png`).
    - Fix: build both pills out by scale steps at their own fills.

19. **[P3] Heart: on rewrist, MEASURING lasts 9 frames.**
    - The only gate is `good_t >= 0.5 s` (`fit_heart.c:534`). When quality returns at once, the
      lock choreography starts 0.3 s after contact (`heart/hr_rewrist.png` f443→f452), so
      MEASURING reads as a flash.
    - Fix: also require 2 s since contact returned (`H.contact_t >= 2.0f`).

20. **[P3] The widget's over-goal ring reads like 35 %.**
    - At 135 % the widget draws the overflow lap in white over a C_SEC first lap, so the eye
      reads a 35 % white arc (`activity3/wg_activity_over.png`). The widget can't use red,
      because the page owns the unread dot.
    - Fix: draw the first lap full white, and the overflow as a second white ring of dots at
      r − 14.

21. **[P3] Primary pills build as blank white, then the label pops in.**
    - ADD ALARM is an empty white pill for 6 frames, then its label appears in 1 frame
      (`alarm/al_entry_empty.png` f10-f16).
    - The same `if (k < 1) return` gating is used for the stopwatch pair and the picker START.
    - Fix: bring the label in from k ≥ 0.6 in 3 steps of `ui_mix(fill, ink, …)`.

22. **[P3] Module naming differs.** The widget says `[ NO HR MODULE ]`; the absent screen's
    footer says `HEART RATE MODULE`. §8 uses one `<NAME>` for both. Use `HR MODULE` in both
    places, which also fits the widget.

23. **[P3] Engine colour residue.**
    - `gfx_blend565` (`core/gfx.c:104`) truncates the 5- and 6-bit lanes separately, so
      anti-aliased edges of white on black pick up green casts.
    - `ui_dim(C_WHITE, 1)` returns 0xFFDF, not 0xFFFF.
    - Fix: round each lane as `(a*w + b*(255-w) + 127) / 255` at its own bit depth, and
      short-circuit k ≥ 1 in `ui_dim`.

24. **[P3] Widget→timer open rebuilds identical text.**
    - The app's `TIMER` title and `5 MIN` label sit in the same place as the widget's, yet they
      sparkle-rebuild over f262-f269 (`wg_timer_open/open.png`).
    - Fix: when entered from the widget, draw the title and total label at build 1, as is
      already done for the digits.

25. **[P3] Alarm editor: the summary line repeats the chips.**
    - `TUE WED THU` is printed 80 px under the TUE/WED/THU chips (`alarm/al_edit_bottom.png`).
    - Fix: show the line only for words the chips can't say (`EVERY DAY`, `WEEKDAYS`,
      `WEEKENDS`, `ONCE`); otherwise leave it empty.

26. **[P3] §8 nits.**
    - Off-standard sizes:
      - DELETE is 196 wide (standard 200).
      - SNOOZE is 170 wide.
      - List pitches are 100 (alarms), 96 (world clock) and 80 (chooser) instead of 90.
      - The stopwatch tenths use pitch 5, which is neither 4.4 nor 3.
    - Declared deviations I accept: stopwatch pair at y 354; TIME'S UP pair at y 316 / w 124
      (forced by the pulse rings); widget timer ring r 158.

## What is already excellent (do not break)

- **Performance and correctness.** 0 late frames in 24,816 frames, and all 12 damage checks
  pass. Static states go idle: alarm list, analog view between seconds, paused timer, Breathe
  holds.
- **Heart.**
  - The lock choreography: the trace descends, the BPM assembles and the heart switches to red
    (`heart_lock2/L.png` f124-f137).
  - Off-wrist and rewrist crossfades.
  - The SIGNAL meter, the `LAST 24 H` page and the module-absent screen.
- **Activity.** Segment-by-segment ring build, the red overflow lap, whole-dot charts, and a
  goal line that sits in a row gap.
- **Breathe.** The 0.18 s build-out on AGAIN, and the outro that collapses the field to a point.
- **Timer.**
  - The pause crossfade.
  - Widget-open keeps the digits.
  - A tight drum rubber band.
  - `6 SEC`, `+1` with `MIN` micro, and 12-level pulse rings.
  - The wg_timer quick-start flash, with no toast.
- **Alarms.**
  - Snooze is coherent end to end (`SNOOZED 07:09`, toggle on, `NEXT IN 9 MIN`), and cancelling
    it crossfades the label and the header (`ring_r2/cx.png`).
  - Sorted rows; the time dims with the toggle knob; list presses hold for 100 ms.
  - Arm-then-tap delete with expiring dots.
  - Slide to stop tracks 1:1, commits with a spring to the end, then drops away
    (`ring_r2/sl.png`).
- **Stopwatch.**
  - The 60 s sweep.
  - The layout spring when the first lap arrives.
  - One x for all lap rows.
  - Paused grey with `[ PAUSED ]`, and RESET fading the completed laps.
- **World clock.** Correct under DST, day/night dots, clean chooser offsets, a quiet analog view
  whose seconds dot builds with the dial, and scroll kept on reveal.
- **Revealed time screens** (stopwatch, timer, world clock, alarms) keep their state with no
  rebuild (`reveal_time/`). This is the model for the #1 fix.
