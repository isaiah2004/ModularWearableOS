# NOS critique: Settings, Modules, About, Diagnostics + consistency, round 5 (final)

**Build:** `build/nos_sim_crit5_sys.exe` at HEAD `23b5981`. Evidence is under `build/nos/critic/r5_system/` (scripts in `scripts/`, output in `out/<script>/`, grids `g_*.png`).
**Cost:** 36 scripts, 0 late frames. The max is 24.4 ms, reached only on full-screen transitions. **Damage check:** PASS on all 36 scripts (31,599 frames; `dmg_all.txt`, `dmg/r5_*`).

## Verdict
**Not met.** Every r4 item and every diag_r1 item is fixed. There is one new P1, in the most common Diagnostics path: a run with fails that ends while the list is on screen. There are also three P2s. Settings and About have nothing above P3.

## system_r4 status: 8 of 8 fixed
1. CC notes now read `NO LTE` / `NO HAPTICS` via `ui_module_short` (`out/n_cc2/cc_note_lte.png`).
2. The speaker wiring line spans x 90–376 (`out/d_mod/d_spk_absent.png`).
3. Modules HR shows `[ MEASURING ]` from f224, the same as the Heart app (`out/n_hr2/m_hr_in.png`).
4. A CC tap now springs to the 10 % step (84→90 %), and the pressed track is shared through `ui_slider_p` (`out/n_tap/cc_tap.png`).
5. Footers read `HR SENSOR MODULE` / `LIGHT SENSOR MODULE`.
6. `ui_module_short` is now the only short-name table.
7. `DIAG_W` is 260.
8. The pin reads `PWRKEY GPIO 18` in both places.

## diag_r1 status: 13 of 13 fixed, plus P3s
- #1–#13 are all fixed.
  - Fails come first, with the first fail row at y 370 (`out/d_flow2/list_fails_first.png`).
  - Card hand-offs fade the cards as they were left (`btn_to_touch.png`, `haptic_to_spk.png`).
  - RE-RUN fades, then the title slides back (`rerun.png`).
  - Wake is held only while a test runs, and a card times out after 30 s (`after_30s.png`, log `NO ANSWER`).
  - Touch ignores x < 40 and touches > 120 px from the dot; a near miss shows `MISSED`.
  - The hero builds directly when the run is already done (`out/d_all/to_list.png`).
  - The count morphs and the long wait is named (`CELLULAR`, `GPS +1`).
  - The summary reads `· 9 NOT CONNECTED`, and absent pages use `ui_absent`.
  - The copy is fixed, PASS rows are C_DIS, and fails get 12 px rings.
  - FB feedback fires at both points.
- P3s: hints are caps, the row page has a `Passed` / `● Failed` hierarchy, the push-dim tint is gone, and the held disc and pill hold through the fade.

## New issues
1. **[P1] Diagnostics: when a run with fails ends with the list on screen, the hero pops in, fades out, then builds again.**
   - f751 shows `[ TESTING ] 12/13 CELLULAR`.
   - At f752 the finished `9/13 PASSED ● 4 FAILED` hero appears at full strength in one frame.
   - f753–757 fade that hero out with the BOARD rows, and f758–768 build it a second time.
   - Cause: during `G.pend` (PEND_RELAYOUT), `draw_list(…, built=1)` → `draw_summary(built=1)` takes the hero branch because `G.done` is already 1.
   - Evidence: `out/r5_finish/fin.png`, `fin_010.png` → `fin_011.png`.
   - Fix: in `draw_summary`, treat `G.pend == PEND_RELAYOUT` as in progress. Use `if (!G.done || G.pend || (!direct && p < FADE_S && !built))`, so the outgoing list fades the TESTING block. The hero then builds only once, after the relayout.
2. **[P2] Modules HR: the state crossfade draws both words on top of each other.**
   - `[ MEASURING ]` and `[ NO CONTACT ]` overlap at y 204 for 3 frames and read as `[[ NMOE ACSOUNRTIANGT ]]` (f255–257, f301–303).
   - `[ MEASURING ]` and `BPM` overlap the same way at f317–319.
   - Evidence: `out/r5_hr/off_in_035.png`, `on_again.png`.
   - Fix (set_modules.c:985–991): fade in sequence instead. Old `k = q8(1 − 2p)`, new `k = q8(2p − 1)`, over 0.24 s. The trace and hint keep the current `p`.
3. **[P2] Diagnostics > Cellular, fail: the readings contradict the verdict.**
   - The page shows `NO AT REPLY IN 25 s` above READING `SIM READY` / `NOT REGISTERED`. If the modem never answered, nothing is known about its SIM.
   - Evidence: `out/r5_lte/lte_detail.png`.
   - Fix (set_diag.c:309–312): when `!l->powered`, set `RD(D_LTE,0,"NO AT REPLY")` and `RD(D_LTE,1,UI_NONE)` before the fail.
4. **[P2] One part has three names.**
   - The Modules and Diagnostics row and page title say `Environment` / `ENVIRONMENT`. The body says "climate sensor". The footer, short name and Baro app say `CLIMATE MODULE` / `NO CLIMATE`.
   - The long name also pushes the row's `NO MODULE` onto the sub-line, so this one row loses its `I2C 0x76 BME280` part line.
   - Evidence: `out/d_mod/mod_absent_l4.png`, `d_env_absent.png`.
   - Fix: rename to `Climate` / `CLIMATE` in set_modules.c:74 and set_diag.c:69.
5. [P3] Diag count: `12/13` (resolved) is followed by the hero `9/13` (passed), the same format with a different meaning. Fix: `12 OF 13` while testing.
6. [P3] A skipped Button or Touch card counts as `FAIL` while its why says `SKIPPED, NOT TESTED`. Fix: why = `SKIPPED` (`out/d_fail/list_done.png`).
7. [P3] Some lines are wider than 320 px:
   - HR hint `WEAR IT SNUG, SENSOR ON SKIN` (390 px, y 296, over the trace band)
   - Touch-card hint (376 px)
   - Motion and Touch reading lines (376 px)

   Fix: `SENSOR ON SKIN`, `PRESS BUTTON TO SKIP`, and split the readings over 2 lines.
8. [P3] The climate wiring line `0x76/77 SDA 47 SCL 48` drops the `I2C` that its part line has. Fix: `I2C 0x76  SDA 47  SCL 48`.

## Already excellent (don't break)
- Arm-then-tap-again on Restart and Reset; the drum edge fades; one absent stack with the same words in every app.
- In Diagnostics:
  - The fails-first list.
  - Clean card hand-offs, and a held YES that keeps C_SEC through the fade.
  - The direct hero on a healthy run.
  - The touch judge that ignores stray taps.
- Modules HR decides its state once and matches the Heart app.
- 31,599 damage-checked frames are pixel-exact, with 0 late frames.
