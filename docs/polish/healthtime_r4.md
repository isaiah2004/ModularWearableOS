# healthtime — critic round 4

Sim `build/nos_sim_crit4_ht.exe` at 0707127. All 39 r1-r3 scripts were re-run into `r4/old/`, plus 11 new
ones in `r4/scripts/`. Paths are relative to `build/nos/critic/healthtime/r4/`. COM3 was not touched.

## Verdict

All ten round-3 items are fixed and nothing regressed. One new **P1** is left. It is in the ring screens'
exit, and the round-3 `leaving` fix made it worse. A second tap while SNOOZE or STOP drops the screen grabs
the falling screen. The screen springs back up and sits frozen for about 2.5 s, until the svc grace pops it.
Besides that there are two P3s. Cost model: 0 late frames in 45,673 frames, worst 24.4 ms (a full-repaint
bus). 19/19 `--damagecheck` PASS (`dc/`). Not done yet: one P1 is above P3.

## Round-3 status

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Hand under pills | Fixed: red px 82→45→20→4→0, 4-6 frames per step, ≥1 px clear of the pill. It is hidden 89 frames (3.0 s) at each pill, the declared trade-off | `old/sw1/ur.png`, `ul_*.png`, `swmin/` |
| 2 | Armed DELETE width | Fixed: outline x 73-392 (320), label 102-361, 29-31 px clear | `old/edit_r2/arm_009.png` |
| 3 | Run-view controls | Fixed: discs scale 66→88 at full colour (✕/+1 #181818), ink in 3 steps from f267 | `old/wg_timer_open/open.png` |
| 4 | Blank primary pill | Fixed: ADD ALARM is blank 2 frames (f6-7), label from the 2nd scale step | `old/alarm/al_entry_empty.png` |
| 5 | SNOOZE / STOP drop out of press | Fixed: they stay C_SEC through the fall | `old/ring_r2/sp.png`, `old/timer_r2/rg.png` |
| 6 | Stale run view under TIME'S UP | Fixed: the presets build under the cover | `old/timer_r2/rg.png` f664-669 |
| 7 | Breathe AGAIN black frames | Fixed: the field seeds at f1958, no empty frame | `old/sumpress/ag.png` |
| 8 | wg_heart absent over a lit chart | Fixed: ghost-only grid, `[ NO HR SENSOR ]` | `old/misc/wg_heart_absent.png` |
| 9 | Chooser pressed row | Fixed: C_LINE2 r 40, ring stays visible | `old/wc/wc_pick_pressed.png` |
| 10 | RESET tenths cut | Fixed: the tenths morph with the digits (f1510) | `old/sw1/rs.png` |

## New issues

1. **[P1] Alarm ring and TIME'S UP: a second tap during the drop brings the ring screen back, frozen.**
   - Tap SNOOZE, then tap again 2 frames later. The screen starts falling (f204-206) and the second tap grabs
     it: `press_begin` takes over any `TXS_AUTO` spring with v > 150 px/s (`nos.c:500`).
   - On release with no travel, `proj > 0.5` springs it back to rest (f207-216). It then sits with SNOOZE
     pressed, `[ SNOOZED ]` up and the pulse ring running. `R.leaving` swallows all input, so the slide, SNOOZE
     and back do nothing.
   - The svc `RING_GRACE_MS` pops it at f279, 2.5 s later (`stuck/db.png`, `z/db_sel.png`). TIME'S UP STOP does
     the same (`tdbl/td.png`, `z/td_sel.png`; frozen for 70 frames).
   - Fix (engine): a pop started by `nos_pop()` outside `finger_release` can't be grabbed. Set
     `T.no_grab = 1` in `nos_pop`; `press_begin` swallows the touch while `T.popping && T.no_grab`, so the drop
     runs on. Belt and braces in both apps: in `ring_tick`, `if (R.leaving && nos_top() == &scr_alarm_ring &&
     !nos_transitioning()) nos_pop();`, and the same with `RG.leaving` / `scr_timer_ring`.
2. [P3] Every back reveal tints C_SURF2 green, then magenta.
   - `dim_rows` scales the 565 lanes separately (`nos.c:683`), so #181818 passes through #101410 and then
     #181418 for 2 frames.
   - Seen on the TIME'S UP→presets reveal (`old/timer_r2/rg_009/010`), on the +1 MIN reveal (`tplus/pl_009/010`,
     `z/pl9.png`) and on chooser→world clock (`back/wb_004/005`).
   - Fix (engine): for neutral pixels (`r5 == b5 && g6 >> 1 == r5`), use `l5 = (r5 * k + 16) >> 5`, green
     `2 * l5 + (l5 >= 16)`, as `ui_mix` does.
3. [P3] Alarm editor confirm-delete: the press shows 2 frames (C_SURF2 f131-132), then the whole editor cuts
   in one frame to `[ DELETED ]` on the departing slide (`delcatch/dl_001`→`dl_002`).
   - Fix: act after `PRESS_HOLD`, like the other pills.
   - Keep drawing the editor from E's copy until `leave`, and let `E.valid` gate only input
     (`time_alarm.c:605`).

## Already excellent (keep)

- The hand gives way in clean 4-step shrinks and grows. Run-view and pill builds at full colour.
- Ring exits hold the press. TIME'S UP STOP and +1 MIN uncover the right view.
- Module absent is coherent: Heart page and widget, and Breathe drops AVG BPM (`bra/`).
- The wg_timer chip drops its press as a slow drag starts (`chip/cd.png` f221).
- Zero late frames, damage-check clean everywhere.
