# NOS critique: outconn, round 4

Sim `build/nos_sim_crit4_oc.exe` (HEAD 0707127). Evidence is in `build/nos/critic/outconn/r4/` (all paths below are relative to it). `old/` and `r3/` hold all 50 earlier scripts, re-run. `new/` holds 11 new scripts (`scripts/r4_*.txt`). `z/` holds montages.

## Verdict
All 13 round-3 items are verified. 12 are fixed; #13 is partly fixed because §8 still lacks the icon-disc row. Performance: 0 late frames in 36,528 frames over 58 runs, worst frame 24.35 ms (bus-bound full repaints). `--damagecheck` passes on 23 scripts (12,232 frames). The steady-state screens are now at the bar. Two new P2s keep this area off "nothing above P3". Both appear only when something outside the app changes the screen: a call or Find watch covering Recorder, or a phone or module dropping while an app is open.

## Round-3 status
| # | Issue | Status | Evidence |
|---|---|---|---|
| 1 | Recorder start/stop hard cuts | **Fixed** | Wave FF→D5→B4→8B→62. Page dots 41→83→BD→FF. Title and hint out over 2 frames, in over 2 (`r3/r3_memo/ms.png`, `mr.png`) |
| 2 | Two reds on DOT while recording | **Fixed** | Seconds dot is white (`r3/r3_memo/ml.png` f151) |
| 3 | Noise meter vs hero | **Fixed** | Hero 49→74→85 with the meter. Residue: new #4 |
| 4 | Torch Bayer entry | **Fixed** | #414041/#838583/#BDBEBD/white, 0 isolated px (`r3/r3_torch/te.png`) |
| 5 | Music widget late press | **Fixed** | #9C9D9C from frame 2 (`r3/r3_wgpress/hp_002`) |
| 6 | C_PRI pink | **Fixed** | Chip text #EEEEEE |
| 7 | REC chip press | **Fixed** | #181818 behind the chip from frame 2, face f400 (`r3/r3_memo/mc.png`) and CC f187 (`new/r4_cc/cp.png`) |
| 8 | 00:13 vs 14 s | **Fixed** | List says `13 s` (`r3/r3_memo/r_memos.png`) |
| 9 | Nav refix/label/arrival | **Fixed** | AGO stays `0:12` (`r3/r3_refix/rf.png`). `[ SEARCHING ]` + `N SATS` (`new/r4_nav/ne.png`). Arrival 7 frames (`na.png`) |
| 10 | Level bubble scale-up | **Fixed** | Full size, fades in (`r3/r3_compass/cl.png` f101-f110) |
| 11 | Call end red trace | **Fixed** | No #391818 (`old/r2_end/en_007`) |
| 12 | Storm stack cramped | **Fixed** | `[ STORM ]` sits under the axis (`r3/r3_storm/s_storm.png`) |
| 13 | LTE gap / icon-disc §8 | **Partly** | Gap is now 16 px (y 299→315). The §8 row is still missing: new #6 |

## Issues (most severe first)
1. **[P2] Recorder under a call or Find watch flips to "stopped".**
   - When the call or Find watch rises over a running recording, the outgoing Recorder draws `RECORDER` / `TAP TO RECORD` / a red record disc / a ghosted clock for the 4 frames it stays visible. Evidence: `new/r4_recover/rc_001→rc_002`, `rf_001→rf_002`.
   - Recording actually continues: 00:08 → 00:11.
   - The `[ RECORDING ]` toast then covers `INCOMING CALL` for the full 1.4 s (`new/r4_mix/rc.png` f240-f251).
   - On reveal the wave restarts empty (`new/r4_recover/rf_000`).
   - Cause: `memo_leave()` sets `R.rec = 0` (`con_memo.c:381`).
   - Fix: keep `R.rec` and track a separate `R.away` flag. Skip the toast when the covering screen is call, find_watch or alarm_ring, because the REC chip already says it. Keep the wave history.
2. **[P2] Losing the phone or a module while the app is open is a one-frame cut.** §8 forbids hard cuts on state changes.
   - Phone off: Music player → `Phone not connected` (`new/r4_off2/mo_001→002`). Find phone `[ RINGING ]` → absent (`fo_001→002`).
   - Module removed (`present`): Navigate, Cellular, Barometer and Recorder all cut in one frame (`new/r4_unplug/un_`, `ul_`, `new/r4_unplug2/ub_`, `um_001→002`).
   - Compass builds its absent page correctly but drops the dial in one frame (`uc_001→002`).
   - Phone back on: Music cuts from `Nothing playing` to a bogus `Unknown track 0:00 · -4:05` for 3 frames (`new/r4_live/mon.png` f214-f216).
   - Fix: one shared path. Old content goes out over 2 steps (~70 ms), then `ui_absent` builds with the icon `DOT_REVEAL_CENTER` and a 4-step text ladder. Keep the player hidden until the title arrives.
3. **[P3] Music ring and times jump.**
   - Prev at 1:16 drops the ring from ~40 % to 0 in one frame (`new/r4_live/mpf.png` f364→f365). Reconnect jumps it from 0 to 1:13 (`mon.png` f216→f217).
   - The remaining time shows the old track's `-4:35` for 2 frames (f368-f369).
   - Fix: tween the ring over 200 ms with `EASE_STANDARD`, and swap the times together with the title.
4. **[P3] Noise flashes the in-between band.** On 49→92, `LOUD / Busy street traffic` appears at full on f787 with no fade-in, then fades out (`r3/r3_memo/mn.png` f787-f789). Fix: label the band the hero is heading to, or hold a band for 300 ms before showing it.
5. **[P3] Torch red gauge can't be read.** On the #9C0000 pill, lit dots are #000000 and unlit dots #290000, so 10 % and 100 % look the same (`new/r4_torch/t_red_low.png` vs `r3/r3_torch/t_red_level.png`). Fix: unlit dots #600000.
6. **[P3] §8 still lacks the icon-disc + caption row** (r2 #18): the 88 px discs in Cellular RADIO/SMS and in Find. Doc only.

## Already excellent (don't break)
- **Recorder.** Start and stop run on one 150 ms clock. A second tap queued mid-stop runs cleanly afterwards (`new/r4_cc/sf.png`). Hero and list lengths agree.
- **Navigate.** The SEARCHING build and count crossfade, the 7-frame fix arrival, and the START TRACK → STOP crossfade (`new/r4_nav/ns.png`).
- **Barometer.** SEA LEVEL picker: ± hit areas are 92×96 px, drag and flick are clean, SET returns with `[ SAVED ]` and no rebuild (`new/r4_baro/bset.png`).
- **Torch.** Flat 4-step fades, including rapid toggles (`new/r4_mix/tf.png`).
- **Call.** A call dropped by losing the phone folds cleanly (`new/r4_off2/cd_`).
- **Absent copy.** One consistent wording in every app.
- **Performance and damage.** 0 late frames, damage check passes.
