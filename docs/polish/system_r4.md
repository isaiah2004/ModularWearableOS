# NOS critique: Settings, Modules, About + cross-OS consistency, round 4

**Build:** `build/nos_sim_crit4_sys.exe` at HEAD `0707127`. Evidence paths are under `build/nos/critic/system/r4/` (scripts in `scripts/`, output in `v4/<script>/`, grids `g_*.png`).
**Cost:** 28 scripts, 0 late frames. The max is 24.4 ms, reached only on full-screen transitions.
**Damage check:** PASS on all 19 scripts checked (21,286 frames; `dmg/damagecheck.txt`).

## Verdict
Round 3 landed in full: all 10 items are fixed. Settings, Modules and About have no P0 or P1. The goal of "nothing above P3" is **not quite met**: three P2s remain, and only one of them is in my own files.
- The control centre still uses its own module-absent words.
- The speaker's wiring line touches the bezel. This was caused by the r3 fix that spaced out the pins.
- The Modules HR page opens on a false `[ NO CONTACT ]` and then hard-cuts between states twice.

## Round-3 status
| # | Status | Evidence |
|---|---|---|
| 1 Outdoor iris lag | Fixed: Baro, Weather and Nav draw at f1–f2, Compass at f2–f3; all four use `nos_enter_t()` | `g_ent_out.png` |
| 2 Absent wording | Fixed in Settings and Modules: rows say `NO MODULE`, notes say `[ NO SPEAKER ]` / `[ NO HAPTICS ]` / `[ NO LIGHT SENSOR ]`, footer is `HAPTICS MODULE`. Still wrong in the control centre (#1 below) | `g_absent.png`, `g_abs4.png` |
| 3 State words | Fixed: `[ SEARCHING ]` + `12 IN VIEW`, `[ SEARCHING ]` / `[ OFF ]` + `SIM READY`, `[ MEASURING ]` + `SIGNAL` | `g_states.png`, `g_hr.png` |
| 4 Drum slicing | Fixed: edge rows y 96–97 / 360–361 read 0 in every frame of 3 picker drags; Set time fades out before the edge | `z_settime_top.png` |
| 5 Auto brightness | Fixed: the row reads `AUTO 70%` | `g_list.png` (l0) |
| 6 Restart pill | Fixed: a 260×88 pill (x 103–362) that springs to the red 320 px armed pill, while Reset greys | `g_end.png` |
| 7 Two brightness controls | Fixed: both are now the 235×50 pill. Their behaviour still differs (#4 below) | `g_cc.png` |
| 8 Two modem pages | Fixed: Settings opens `scr_cell`, both with and without the module | `g_misc.png`, `g_abs5.png` |
| 9 Row values | Fixed: `60%`, `70%`, `26°` | `g_misc.png` |
| 10 Wiring line | Fixed: pins are spaced and drawn in C_DIS (98,97,98), the same as the footer. This caused #2 below | `v4/d_mod/d_spk_absent.png` |

## Issues
1. **[P2] Control centre, absent tiles: the notes break §8.**
   - They read `NO MOTOR`, `NO LTE MODULE` and `NO GPS MODULE` (sys_control.c:401). Settings and the toasts say `NO HAPTICS`, `NO LTE` and `NO GPS` for the same parts.
   - Evidence: `g_cc2.png` (`NO MOTOR`, `NO LTE MODULE`), `g_cc.png` (`NO GPS MODULE`).
   - Fix: `snprintf(n, sizeof n, "NO %s", ui_module_short(k_tile[i].need))`.
2. **[P2] Modules > Speaker, absent: the wiring line is 2.2 px from the bezel.**
   - `MAX98357A  BCLK 1 LRC 2 DIN 5 SD 6` spans x 48–417 at y 361–371. The next widest lines are LTE and Light, which keep 19 px.
   - Evidence: `v4/d_mod/d_spk_absent.png`.
   - Fix: keep wiring lines within 320 px (x 73–393 at y 372). Here, drop the chip name (`BCLK 1  LRC 2  DIN 5  SD 6`); the chip is already on the row's part line.
3. **[P2] Modules > Heart rate entry: a false state, then two hard cuts.**
   - f224–f238 show `[ NO CONTACT ]` + `WEAR IT SNUG, SENSOR ON SKIN`.
   - At f239 the hint vanishes and the trace appears at full strength in one frame, under a plain `BPM` with no state.
   - At f253 `BPM` becomes `[ MEASURING ]` in one frame.
   - The Heart app shows `[ MEASURING ]` from its first frame on the same sensor feed (f310).
   - Cause: the draw branch reads `V.hr.on_wrist` live (set_modules.c:979/998), but the unit word is set on the slower detail update (:723–725).
   - Evidence: `v4/n_hr2/m_hr_in.png`, `v4/n_hr2/heart_in.png`, `g_hr_seq.png`.
   - Fix: decide the state once per tick into one field that both use. Show `[ MEASURING ]` from entry, as the Heart app does, until contact has been missing for at least 1 s. Cross-fade the state slot over 8 levels.
4. **[P3] The "one" brightness slider still has two behaviours.**
   - A tap on the control centre jumps to the exact percent in one frame (fill end x 294→323, f1→f2, `32%`).
   - The same tap in Settings springs over 5 frames to a 10 % detent (323→330, `30%`).
   - While pressed, the control-centre track lifts to C_LINE2 (49,48,49), but the Settings track stays at (24,24,24).
   - Evidence: `v4/n_tap/`, `g_cc.png`, `g_cc2.png`.
   - Fix: use the Settings tap (a SPRING_SNAPPY spring to `round(v/10)*10`) in sys_control.c:472–475, and move `draw_slider`'s pressed track into `ui_slider(…, pressed)` for both.
5. [P3] Footer names differ from the short names: `HR MODULE` vs `[ NO HR SENSOR ]`, and `LIGHT MODULE` vs `[ NO LIGHT SENSOR ]` (set_modules.c:93/95, fit_heart.c:638). Fix: `HR SENSOR MODULE` / `LIGHT SENSOR MODULE`, or shorten the short names to `HR` / `LIGHT`.
6. [P3] There are two short-name tables: `set_modules_short()` (set_modules.c:58, used by set_settings.c:458) and `ui_module_short()` (ui.c:702). Fix: delete the first and call `ui_module_short`.
7. [P3] The list-end pills have different widths: RUN DIAGNOSTICS is 300 (x 83–382) and Restart/Reset are 260. The 208 px label fits in 260 with 26 px on each side. Evidence: `v4/d_mod/mod_absent_l5.png`, `g_end.png`. Fix: `DIAG_W 260`.
8. [P3] The same pin is written two ways: `PWRKEY ON IO18` (LTE off, set_modules.c:690) and `PWRKEY 18` on the wiring line, while other pins use `GPIO 16`. Fix: `PWRKEY GPIO 18` on both.

## Consistency (re-measured)
- **Absent pages:** body and footer are identical between the apps and Modules (Cellular from Settings = Modules minus the wiring line).
- **State slots:** all go through `ui_state`.
- **Toasts:** 16 strings in the source, all fit.
- **Entry builds:** f1–f2 OS-wide; Compass f2.
- **Red:** ≤ 1 on every settled screen. While one action is armed the other greys.
- **Pressed states:** secondary pills (Restart, ADD ALARM, CUSTOM, START) all press to C_SEC with dark ink.

## Already excellent (don't break)
- Arm-then-tap-again: Restart and Reset are now identical to the power sheet (260→320 spring, red cross-fade, 3 dots).
- The fade-out drum edges.
- One modem page, one radio page, and one absent stack, shared word-for-word.
- 21,286 damage-checked frames pixel-exact; no late frame.
