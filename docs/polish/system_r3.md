# NOS critique: Settings, Modules, About + cross-OS consistency, round 3

**Build reviewed:** `build/nos_sim_crit3_sys.exe` at HEAD `65754b3`. It includes 6e4575e (press drops after 8 px) and d00442b (8 px unread dot). The Diagnostics app (set_diag.c) is out of scope.
**Evidence:** paths are relative to `build/nos/critic/system/r3/`: scripts in `scripts/`, per-script output in `v3/<script>/`, grids `g_*.png`, zooms `z_*.png`, tools in `tools/`.
**Cost:** 16 scripts, 0 late frames. The max is 24.4 ms, and only full-screen transitions reach it.
**Damage check:** PASS on all 15 checked scripts (20,248 frames, `dmg/damagecheck.txt`).
**Toasts:** all 16 toast strings in the OS were replayed (`v3/x3`). None drops below the top slot; the only warnings are the sim's own `(SIM)` toasts.

## Verdict
Round 2 landed almost in full: 23 of 24 items are fixed and 1 is not fixed (#20). The two P1s are gone:
- The GPS keeps its fix under the detail page.
- The picker is now a true 1:1 drum. On a 100 px drag the ruler moves exactly 100 px with monotonic steps, then settles 6 px to the detent with no overshoot.

Settings, Modules and About now build from f1–f2, share one radio page with one word per state, keep the toast slot, and grey absent toggles. Nothing in my area is above P2. Three P2s remain, and all are consistency issues:
- Four outdoor apps still carry the hard-coded iris lag that round 2 removed from Settings.
- The module-absent state has six different wordings.
- State slots mix bracketed and plain words, and the same state has different names in Modules and in the apps.

## Round-2 status
| # | Status | Evidence |
|---|---|---|
| 1 GPS off under detail | Fixed: list `11 SATS` → detail 11/10/9 `3D FIX` → list `9 SATS` | `g_gps.png` |
| 2 Picker sawtooth | Fixed: ruler 107→7 for a 100 px drag, steps 15,7,9,12,14,12,9,7,6,4,2,2,1, then a 6 px settle | `v3/b_sub/pick_drag_*.png` |
| 3 Settings builds late | Fixed: rows appear at f1–f2, the same as Modules and About | `g_ent_settings.png` |
| 4 Power sheet two reds | Fixed: arming one action greys the other; Restart morphs into the 320 px pill over 4 frames | `g_power.png`, `z_power_morph.png` |
| 5 ADD ALARM blank frame | Fixed: 4 pressed frames, then a clean push | `g_press.png` |
| 6 Long toasts | Fixed: every toast string fits the top slot | `v3/x3` log |
| 7 Values squeeze labels | Fixed: names hold x 85, gaps ≥ 46 px, long values move under the label | `g_mods.png`, `g_absent.png` |
| 8 Radio words / pages | Fixed: one page, `CONNECTED` everywhere, Modules adds the bus line | `g_radio.png` |
| 9 Cellular `OFF` hero + wiring | Fixed: `--` / `[ OFF ]`, no PWRKEY line from Settings | `v3/c_sub/cell_from_settings.png` |
| 10 Lux | Fixed: `LUX` in the list, the detail and `320 LUX NOW` | `g_pickers.png` |
| 11 Live toggles with no module | Fixed: greyed; a tap saves and toasts `[ NO SPEAKER ]` | `g_x3b.png` |
| 12 Absent copy differs | Fixed: Modules uses the app's body and footer, plus a wiring line | `g_absent.png` |
| 13 Arm pill cuts | Fixed: width springs 260↔320 over 6 frames (f528–533, f619–624); red cross-fades | `v3/a2_arm/arm_expire.png` |
| 14 Slow drag flashes pill | Fixed: pill shows f2–f5 and drops at f6 (8 px), before the list moves at f7 | `g_quick.png` |
| 15 Radio state y / off-white tile | Fixed: same y; tile #FFFFFF | `g_radio.png` |
| 16 Storage title | Fixed: `STORAGE` | `v3/c_sub/storage_0.png` |
| 17 Motion hero crowds | Fixed: 17 px gap, the same as SD and Battery | `tools/bands.py` |
| 18 About | Fixed: `ABOUT` title, tagline with row 1, glint swells, `4.00` + `V` label | `g_about.png`, `z_glint.png` |
| 19 Timer ring / Stopwatch pairs | Fixed: both 140×88 at x 154/312 | `tools/blob.py` |
| 20 Two brightness controls | **Not fixed** (#7 below) | `g_cross.png` |
| 21 Unread dot | Fixed: 8 px on both; white on a face that already has red | `g_cross.png` |
| 22 AOD entry cut | Fixed: dims f0–f3, AOD face rises f4–f6 | `v3/h_aod/aod_enter.png` |
| 23 `STRAP 26°` / 24 notif header | Fixed: `ON WRIST 26°`; header at x 67 | `g_cross.png` |

## Issues
1. **[P2] Barometer, Compass, Navigate and Weather build 0.17 s late. This is round-2 #3 in four more apps.**
   - Each sets `t0 = nos_t + IRIS_LAG` with `IRIS_LAG 0.17f`, on top of the 40 % iris point the engine already reports: out_baro.c:257, out_compass.c:162, out_nav.c:332, out_weather.c:510.
   - First hero pixels appear at f5 (Baro, Weather) and f7 (Compass), against f1 for Activity.
   - Compass is empty f1–f3 and shows only its red marker at f4 (`g_ent_late.png`, `v3/i_tour/e_compass.png`). Navigate shows only its title until f5.
   - Push entries, which have no iris, wait 0.17 s too.
   - Fix: `t0 = nos_enter_t();` in all four, and delete the define.
2. **[P2] The module-absent state has six wordings, and §8's own rule can't fit a toast.**
   - Settings Cellular row: `NO MODULE`. Modules strap rows: `ABSENT`. Pages: `Not connected`.
   - Brightness note: `[ NO LIGHT MODULE ]`. Volume note: `[ NO SPEAKER ]`. Haptic strength note and toast: `[ NO MOTOR ]`, while the same part is `Haptics` / `HAPTIC MODULE` and its body says "Add the motor…" (`g_modabs.png`, `g_absent.png`).
   - §8 prescribes `[ NO <NAME> MODULE ]`, which only Brightness follows and which can't fit the ~10-caps toast slot.
   - Fix:
     - Rows (Settings and Modules) say `NO MODULE`.
     - Notes and toasts say `NO <NAME>` from one table: SPEAKER, HAPTICS, GPS, LTE, LIGHT SENSOR (note only), MIC.
     - Rename the footer to `HAPTICS MODULE` and the body to "Add haptics to feel every tap".
     - Amend §8 to match.
3. **[P2] State slots mix bracketed and plain words, and the same state has two names.**
   - Cellular detail: `[ OFF ]` / `[ STARTING ]` are bracketed, but `SEARCHING`, `NO SIM` and `SIM LOCKED` sit plain in the same slot (`v3/d_mod/d_lte_on.png`; set_modules.c:640).
   - HR detail: `LOCKING ON` plain, with hero `--` and an unlabelled quality bar. Heart app, same state: `[ MEASURING ]` with `SIGNAL` (`g_hr.png`).
   - GPS detail: `8 IN VIEW, NO FIX YET`. Navigate: `[ SEARCHING 4 SATS ]` (`g_apps.png`).
   - `WAITING FOR A SAMPLE` (Environment, Compass) is plain.
   - Fix:
     - Every state in `unit_line` goes through `ui_state` (`D.ustate = 1` for all non-unit words).
     - Use the app's words: `[ MEASURING ]` plus a `SIGNAL` label on the HR segments, and `[ SEARCHING ]` with `8 IN VIEW` under it for the GPS.
4. [P3] Picker drum: an incoming value is sliced by a hard edge at y 351 for 2 frames (`60` reads as "cn"/"Al", f259–260, `g_pick_clip.png`), and at y 104 on the way up. Fix: fade a value to 0 by the time its glyph reaches the window, e.g. `ui_dim(C_DIS, q8(1-(a-1)/0.25))`, or grow the window to 96–362.
5. [P3] With Auto brightness on, the list row says `AUTO` but the page hero says `70%` (`v3/a_list/l0.png` vs `v3/b_sub/bright_0.png`). Fix: draw the hero in C_SEC with a `BASE LEVEL` label while Auto is on, or make the row read `AUTO 70%`.
6. [P3] Settings Restart is a bare text row next to the Reset pill, while the power sheet's Restart is a pill (`v3/a_list/l7.png` vs `v3/f_power/power_0.png`). Fix: a 260×88 C_SURF2 pill that arms with the sheet's morph.
7. [P3] (r2 #20) The Control centre brightness is still 10 segments; Settings uses the 235×50 pill slider (`g_cross.png`). Fix: one control.
8. [P3] The modem has two power UIs: the Cellular app (bars hero, round RADIO tile, `TAP RADIO TO TURN ON`) and Settings > Cellular (`--` hero, POWER ON pill) (`v3/i_tour/t_cell.png` vs `cell_from_settings.png`). Fix: the Settings row opens `scr_cell`; Modules keeps the bring-up page.
9. [P3] Modules row values drift:
   - Speaker `VOL 60%` vs Haptics `70%`.
   - Environment `26 °C` (spaced) vs `26°` on Weather and `26.0°` on the detail (`g_mods.png`).
   - Fix: `60%`, `26°`.
10. [P3] Absent wiring line:
    - Speaker pins are formatted `BCLK1 LRC2 DIN5 SD6`, against `BCLK 1 WS 2 SD 3` on the mic.
    - The wiring line (C_SEC) is brighter than the module footer (C_DIS) below it (`v3/d_mod/d_spk_absent.png`).
    - Fix: space the pins, and draw the wiring line in C_DIS.

## Consistency tables
- **Titles:** cap band y 58–69, C_SEC on every pushed screen and widget page (`tools/measure.py` over 60 shots). Exempt: faces, Compass, Torch, Activity, sheets, rings, Touch test (y 120, by design).
- **Buttons:**
  - Singles are 200×88 at y 372: ADD ALARM, SET, RESYNC, POWER ON/OFF, CALIBRATE.
  - Pairs are 140×88 at x 154/312: notification OPEN/DISMISS and Stopwatch at y 372; Timer ring at y 316 (the ring owns the bottom).
  - Power sheet: a pair at y 272, single at y 372. Chips are 56 tall (Breathe, Timer).
- **Pressed states:** ≥ 3 frames on Settings, Modules and play rows (pill from f2, held 0.10 s), ADD ALARM (4), SET, and the radio tile. Rows drop the pill at 8 px of travel.
- **Absent pages:** one `ui_absent` stack everywhere, with matching body and footer between the apps and Modules; Modules adds the wiring line. Words differ: #2.
- **State words:** radios are unified. Modem, HR and GPS are not: #3.
- **Units:** `V`, `dB`, `hPa`, `m`, `g`, `°/s`, `uT`, `km`, `GB`, `LUX` keep their case. Exception: `26 °C` (#9).
- **Toasts:** bracketed caps pill at the top; all 16 strings fit. Casing in the source varies (`List full`, `Snoozed`, `Opened`); it renders as caps.
- **Entry builds:** f1–f2 for system, time, fit and connect apps. Outdoor apps start at f5–f7 (#1). Pushes build from `nos_enter_t()`.
- **Back:** engine swipe works on every pushed page, including the SCR_HDRAG pages (slider, Touch test) from the edge.
- **Red:** ≤ 1 element on every settled screen measured. While one action is armed the other greys, on both Settings and the sheet.

## Already excellent (don't break)
- The drum: 1:1 tracking, sized and lit by distance, rulers that turn with it, no-bounce snap.
- Arm-then-tap-again: the width spring, the red-to-grey cross-fade and the 3-dot countdown are identical on Settings and the power sheet.
- One radio page shared by Settings and Modules, with one word per state.
- Absent copy is shared word-for-word between the apps and Modules.
- The GPS fix survives a detail round trip.
- About: logo-first build, a glint that swells instead of dimming, dots only for live values.
- 20,248 damage-checked frames pixel-exact; no late frame anywhere.
