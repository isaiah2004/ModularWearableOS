# healthtime — critic round 3

Scope: Activity, Heart rate, Breathe, Timer (+ custom picker, TIME'S UP), Stopwatch, Alarms (list,
editor, ring, snooze), World clock (list, chooser, analog), widgets wg_activity / wg_heart / wg_timer.

Sim: `build/nos_sim_crit3_ht.exe` at d634733 (round-2 fixes 77fee72, f04b390, 9426686, plus the
engine's b4f6492). All 38 round-1/2 scripts re-run into `build/nos/critic/healthtime/r3/old/`,
plus the new `r3/scripts/sw1.txt` (stopwatch, red hand vs pills) → `r3/sw1/`. Paths below are
relative to `build/nos/critic/healthtime/r3/`. COM3 untouched.

## Verdict

Round 2 landed: 25 of 26 items are fixed, 1 is partly fixed, and nothing regressed. Revealed
screens keep their state (heart stays locked through a power-sheet reveal, Activity stays on the
week page). Toasts fit the top slot (`[ SNOOZED ]`, `[ LIST FULL ]`; no sim "too long" warning in
39 runs). Every control I tapped shows its press for at least 3 frames in C_SEC. The departures,
the chips and DELETE arming are stepped, not cut. Cost model: 0 late frames in 25,849 frames,
worst 24.4 ms (the bus cost of a full repaint). 14 damage checks PASS (`dc/`). Four P2s are
left, all small and all with exact fixes: the stopwatch hand clipping under the pills (the
declared trade-off is worse than "a few frames"), the armed DELETE width, blank or grey-fading
timer run controls, and the remaining blank primary pill. No P0 or P1.

## Round-2 status

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Heart/Activity reset on reveal | Fixed: 70 BPM stays locked, history and week pages kept | `old/reveal/hr_reveal.png`, `act_reveal.png`, `cmp_reveal.png` |
| 2 | Toasts too wide | Fixed: `[ SNOOZED ]`, `[ LIST FULL ]` | `old/ring_r2/so.png`, `cmp_wc.png` |
| 3 | Presses 2 frames / C_SURF3 | Fixed: ≥3 frames in C_SEC (START, presets, pause, STOP, LAP, RESET, chips, SNOOZE); DELETE presses to C_SURF2 | `old/press/st.png`, `old/timer_r2/go.png` f29-31, `pz.png` f62-64, `sw1/lp.png` f1169-71 |
| 4 | Timer departure in 2 frames | Fixed: tapped pill held, 4 alpha steps f32-f35 | `old/timer_r2/go.png` |
| 5 | One-frame cuts | Fixed: chip f72-74, DELETE spring/crossfade/dots f155-161, hand sweeps home f1508-12 | `old/edit_r2/ch.png`, `arm.png`, `sw1/rs.png` |
| 6 | DAILY AVG includes today | Fixed: 7836 | `cmp_reveal.png` |
| 7 | Breathe summary 14 px from pair | Fixed: 30 px | `old/sumpress/sum.png` |
| 8 | Track touches pulse ring | Fixed: cap far point r 189, 18 px clear | `old/ring_r2/ring.png` |
| 9 | No hint of pages below | Fixed: scrollbar flashes at f12 | `old/heart/hr_entry.png`, `old/activity/act_entry.png` |
| 10 | First lap overlaps digits | Fixed | `sw1/z_lp.png` |
| 11 | Lap list rests between rows | Fixed: snaps to LAP 03/02/01 | `sw1/z_list.png` |
| 12 | Red tip blinks | Fixed: a full dot moving smoothly | `old/timer_tip/tip.png` |
| 13 | Grabber on ring screens | Fixed: none on NOBACK | `old/ring_r2/in.png` |
| 14 | Ring entry fade-ups | Fixed: scale steps at full colour | `old/ring_r2/in.png` f180-187 |
| 15 | LAST while live | Fixed: `NOW 71 BPM` | `old/heart/hr_to_hist.png` |
| 16 | Hero morph >1 Hz | Fixed: `MORPH_GAP 1.0` (code) | `fit_activity.c:149` |
| 17 | Scrollbar fade uneven / pop | Fixed: `qz(bar_k, 8)` drawn to the last step | `old/sbfade2/` |
| 18 | DONE dims through grey | Fixed | `old/sumpress/ag.png` |
| 19 | Rewrist MEASURING flash | Fixed: ≥2 s | `old/heart/hr_rewrist.png` |
| 20 | Widget 135 % reads 35 % | Fixed: inner ring at r−14 | `cmp_wg.png` |
| 21 | Blank pill, then label pops | **Partly fixed.** Pills use `ink_k`, but ADD ALARM is still blank for 4 frames and the timer pause disc for 5 (new #3, #4) | `old/alarm/al_entry_empty.png` f6-9 |
| 22 | Module naming | Fixed: `HR MODULE` in both places | `fit_heart.c:638` |
| 23 | Colour residue | Fixed: neutral blends; `ui_mix` returns b at t ≥ 1 | b4f6492 |
| 24 | Widget→timer rebuilds text | Fixed | `old/wg_timer_open/open.png` |
| 25 | Summary repeats chips | Fixed: `ONCE` only | `old/edit_r2/arm.png` |
| 26 | §8 sizes | Fixed: DELETE and SNOOZE are 200 wide, list pitch 90, tenths pitch 4.4 | code |

## New issues

1. **[P2] Stopwatch: the red hand clips under the pills for much longer than "a few frames".**
   - The pills reach r 218, so the r 211 hand passes under the right pill's cap (θ 136-150°)
     and the left pill's cap (θ 210-224°).
   - It moves 0.74 px/frame, so going under takes about 25 frames. The red pixel count falls
     75→3 over f707-f730 (`sw1/ur.png`, `sw1/z_ur.png`): a red crescent cut by the white STOP
     pill.
   - Per minute that is about 3.3 s of crescent (4 edges), plus 2 × ~1.6 s with the hand
     hidden. A stopwatch stopped near 0:23 or 0:36 rests with no hand visible.
   - Fix: let the hand give way the way the track dots do. In `sw_draw`, compute the hand's
     distance `d` to the nearest pill (the same SDF as `sw_track`) and draw it at
     `10 * qz(m_clamp((d - 2) / 8, 0, 1), 4)`. It shrinks to nothing in 4 steps before the edge
     and grows back after it, and never overlaps a pill.
2. **[P2] Alarm editor: the armed DELETE pill is 284 wide; the arm standard elsewhere is 320.**
   - The `TAP AGAIN TO DELETE` label spans x 102-361 inside an outline whose inner edge is at
     93/372. That leaves 9-11 px at each end, with the text running into the 44 px caps
     (`old/edit_r2/arm_009.png`).
   - Power and Settings arm to 320 (`sys_power.c:26`, `set_settings.c:640`).
   - Fix: `E_ARM_W 320.0f`. The hit test already uses `E_ARM_W`.
3. **[P2] Timer run view: the controls build against §8.**
   - The pause disc builds as an empty white disc, and its icon appears only at `k >= 1`: 5
     blank frames on opening from the widget (`old/wg_timer_open/open.png` f265-f269), and on
     every preset start (`old/timer_r2/go.png` f41-f42).
   - ✕ and +1 fade up from black, with their ink coming up from grey: `ui_dim(C_SURF2,k)` and
     `ui_dim(C_PRI,k)`, f266-f269.
   - Fix: draw the icon with `ui_mix(disc, C_BG, ink_k(bp))`. Draw ✕/+1 at full C_SURF2,
     scaled `40 * build_scale(bp)`, with their ink at `ink_k(bp)`.
4. **[P2] ADD ALARM (empty list) is still a blank white pill for 4 frames** (f6-f9, label at
   f10) (`old/alarm/al_entry_empty.png`). The same `ink_k` gating (label from k 0.6) applies to
   ADD CITIES and the picker START. Fix: `ink_k(k) = qz(clamp((k - 0.3)/0.4, 0, 1), 3)`, so
   the label starts on the second scale step.
5. [P3] Alarm ring: a deliberate tap (≥ 0.1 s) on SNOOZE fires at once, and the IN_UP after it
   re-arms `snz_unpress`. The pill drops back to C_SURF2 as the screen falls (`old/ring_r2/sp.png`
   f287); TIME'S UP holds STOP (`rg.png` f658-666). Fix: set `R.leaving = 1` in `snooze()` and
   ignore IN_UP while it is set. Check `ring_act` the same way.
6. [P3] TIME'S UP → STOP: the drop uncovers the stale run view (`00:00`, `6 SEC`, ✕/pause),
   which then departs before the presets build (`old/timer_r2/rg.png` f664-f669). Fix: on
   reveal with a changed state, `timer_set_view(v, 1)` with no departure. The presets then build
   as the cover lifts.
7. [P3] Breathe AGAIN: 2 pure-black frames between the summary build-out and the field grow-in
   (`old/sumpress/ag.png` f1959-f1960). Fix: start `session_begin(1)` at `T_SUMOUT - 0.067`,
   or floor the first ring's grow at 0.15.
8. [P3] wg_heart with the module absent shows `--` and `[ NO HR MODULE ]` over a lit 24 h chart
   (`old/misc/wg_heart_absent.png`). Fix: when `!WH.present`, draw the columns ghost-only.
9. [P3] World-clock chooser: the pressed row is C_SURF2 (#1A1A1A) at r 32, fainter than the
   C_LINE2 cards (`cmp_wc.png`). Fix: C_LINE2, r 40.
10. [P3] Stopwatch RESET: the tenths cut `.8`→`.0` in one frame while the seconds morph
    (`sw1/rs.png` f1507→f1508). Fix: morph the tenths cell with the digits on RESET only.

## Already excellent (keep)

- Reveal behaviour across all seven apps; the heart keeps its lock, its trace and its NOW
  footer.
- The press holds and the departure model: a flat alpha in 6 steps, the tapped control held.
  The chip and DELETE choreography. Toasts that fit.
- Stopwatch: first-lap layout, row-snapped lap list, the 5-frame hand sweep home on RESET.
- Ring screens: no grabber, scale-step builds, the slide track clear of the pulse ring, a
  coherent snooze.
- Heart: lock choreography, a 2 s MEASURING on rewrist, the scrollbar flash. Activity: DAILY
  AVG over completed days, the over-goal widget ring.
- Zero late frames and damage-check clean throughout.
