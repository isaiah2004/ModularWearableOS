# Shell critique, round 4

Sim `build/nos_sim_crit4_shell.exe` at 0707127. Images are under `build/nos/critic/shell/r4/`, and the scripts are in `r4/scripts/` (all r3 scripts plus `r4_banner r4_dbl r4_rec r4_rec2 r4_cc r4_picker r4_push r4_last r4_music`). The `fNNN` frame numbers come from the contact sheets. COM3 was not touched.

## Verdict
Round 3 is almost fully landed: 10 of 12 items are fixed, and the other 2 are reduced to small residues.
- Exits now run at constant speed.
- The banner arrives and leaves on tweens.
- There is one red while recording.
- A double press no longer reverses.

Nothing is P0 or P1. **One P2 remains**, so the shell does not yet meet "nothing above P3": every transition the code starts without a finger (a push, the power sheet rising, the picker's cancel, a list gap closing) still settles on SPRING_PAGE. Each one ends in a 6-frame creep of full repaints. Performance is clean: 0 late frames over 41 runs, and `--damagecheck` PASSES on 18 scripts (`dc_*`).

## Round-3 status
| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Two reds while recording | **Fixed.** DOT's centre dot and GLYPH's seconds dot are white while the chip is red. | `r3_rec/face_rec.png`, `glyph_rec.png` |
| 2 | Double press reverses | **Fixed.** The iris rim is continuous (x 92, 58, 35, 20, 10 at row 300). Residue: new #3. | `r2_double/dbl.png` |
| 3 | Pressed flash on slow drags | **Partly fixed.** Now 1 frame each: card f241, swipe f273, launcher f406, Timer chip f739 (was 1-4). New #2. | `r3_pressflash/sc,sw,ls,pw.png` |
| 4 | Banner brakes / creeps | **Fixed.** Arrives in 9 frames (+67, 34, 20, 13, 9, 5, 3, 1). Leaves in 6 accelerating frames (bottom edge 187→84, then gone). The toast follows at f189. | `r4_banner/bi.png`, `r3_toast2/tq2.png` |
| 5 | Button close hesitates | **Fixed.** CC, notifications and power all move 70-71 px every frame. | `r3_btnclose/ccb,ntb,pwb.png` |
| 6 | CLEAR ALL black frames | **Fixed.** The bell builds at f274, under the last card. | `notifs3/nt_clear.png` |
| 7 | Banner tap has no pressed state | **Fixed.** C_LINE2 (49,48,49) at f239-240, then the card flies up. | `r4_banner/bt.png` |
| 8 | CC header off-centre | **Fixed.** Ink spans x 168-296, centre 232. | `control/cc_open.png` |
| 9 | Power commit blank frame | **Fixed.** The two labels overlap at f211-212. | `r3_power/com_o.png` |
| 10 | Picker has no cancel | **Fixed.** The button springs back to DOT and saves nothing; a tap chooses. Residue: #1. | `r4_picker/pc.png`, `pch.png` |
| 11 | Green AA tint | **Fixed.** Pixels 2 or more G steps high fell from 254 to 123, all on bright AA such as (222,226,222). The dim path now tints instead (#5). | `r2_reveal/nt_list.png` |
| 12 | Music playhead pops in | **Fixed.** It goes grey to white over f110-111. A new music glyph issue is #9. | `r4_music/mu.png` |

## Issues
1. **[P2] Transitions without a finger settle on a spring, ending in a creep.** Each case below ends in about 6 frames of 1-7 px movement, all full repaints at 24.4 ms. This breaks the §3 rule that non-interactive motion uses `mtween`.
   - **Push into the detail** (from a banner tap or a list tap). The OPEN pill's left edge moves -73, -54, -37, -26, -17, -12, -7, -4, -3, -2, -1, -2 (f243-f255). Evidence: `r4_push/pt.png`, `r4_banner/bt.png`.
   - **Power sheet rise.** The POWER OFF top goes 371, 353, 345, 340, 336, 334, 333, 331 (f354-361). Evidence: `r4_push/po.png`.
   - **Picker cancel.** It moves 49 … 12, 7, 4, 3, 2, 1, 1 (f105-119), then fades; the button-to-home cancel takes 0.63 s. Evidence: `r4_picker/pc.png`.
   - **Gap close after DISMISS.** The card top goes 154, 136, 124, 116, 110, 106, 104, 102, 102, 101, 101, 100 (f255-266). Evidence: `r4_push/pd.png`.
   - **Fix:**
     - in `tx_begin` (nos.c), when no finger velocity is handed over, set `use_tween` for TX_PUSH/RISE/DROP opens: `mtw_start(.., 0.34f, &EASE_STANDARD)`;
     - keep the spring only in `finger_begin` and on release;
     - in `sys_faces.c`, the cancel uses a 0.30 s EASE_STANDARD tween instead of `settle_to`;
     - the notification gap close uses a 0.25 s EASE_STANDARD tween.
2. **[P3] A slow drag still flashes the pressed state for 1 frame** (r3 #3 residue).
   - The press lights while travel is ≤ 8 px but is only dropped once travel passes 8 px.
   - **Fix:** while the finger is down, light the press only if `nos_touch_travel() <= 3`; once lit, drop it at > 8 px. In `pr_visible` (sys_home.c), remove `|| t >= 0.09f`.
3. **[P3] A double press shows the launcher list, then cuts to a nearly black app inside the iris.**
   - With the sim's double press (200 ms), the list shows at f140-141; with a 300 ms double press, it shows for 5 frames, f140-144, and cuts at f145. Evidence: `r2_double/dbl.png`, `r4_dbl/dbs.png`.
   - The launcher's rows are readable from the iris's 2nd frame, which breaks §8 "iris entries build at 40 %".
   - **Fix:** in `nos_retarget`, keep `old` and draw it under the new screen through `nos_ink_dim` at k = 2/3, 1/3, 0 over 3 frames. Alternatively, make the launcher honour the iris build delay.
4. **[P3] The CC header note hard-cuts.**
   - On a tap, the clock vanishes in one frame while the note fades in at 89 → 146 → 238 (`r4_cc/st_003`).
   - On a slider drag, the note pops in at full on the first frame (`sl` f69), because `note_hold` forces age to 0.10.
   - When the note ends, the clock pops back at full in one frame (`st_050`).
   - **Fix:**
     - only clamp while the note is held: `if (C.note_hold && nos_t - C.note_t > 0.10f)`;
     - draw the clock at `kc*(1-k)` during the note fades, and bring it back over 3 steps.
5. **[P3] Transition dimming tints greys magenta.**
   - `dim_rows` rounds the G lane on its own, so C_LINE2 (6,12,6) becomes (49,44,49) and C_SURF2 cards become (24,20,24).
   - This covers 30-47k px over f213-215 and reads as mauve cards (`r4_push/pt_002`, `pt_003`; also `pd` f256-257).
   - **Fix:** for neutral source pixels (`g>>1 == r`), set `g' = 2*r' + (g&1)`.
6. **[P3] The picker's rim jumps at the hand-off.**
   - Rim levels are 16, 16, 16, 40, 40, 40, 56, 56, 114, 138, 157. The 74 and 98 steps are gone since the picker started opening in place (`r4_picker/po.png`, f33-43).
   - **Fix:** the picker's rim continues from home's current level, at the same rate.
7. **[P3] The AOD REC chip blinks out on wake.**
   - Its brightness goes 156 in AOD → 0 at f213 → 57, 57, 123, 180 (`r4_rec/awr.png`). Entering AOD is clean (`r4_rec2/aer`).
   - **Fix:** start the awake chip at the AOD chip's level, or keep drawing the AOD chip until the awake chip passes it.
8. **[P3] "Last app" only counts launches from the launcher.**
   - After opening Heart rate from its widget, a double press opens Activity (`r4_last/frames.csv`: f195 HEART RATE, f287 ACTIVITY).
   - **Fix:** set `s_last` on entering any `nos_app_list` screen, whatever opened it (widget, complication, OPEN, REC chip).
9. **[P3] (cross-team, `con_music.c`) The music page swaps its glyph as it slides in.**
   - It shows play at f104, an empty disc at f105, then pause builds over f106-107 (`r4_music/mu.png`).
   - **Fix:** read the playing state before the first draw.

## Already excellent (don't break)
- **Button exits** run at a constant 70-71 px/frame from the first frame.
- **Banner** arrives on EASE_STANDARD, leaves on EASE_ACCEL, has a pressed card, and toasts queue behind it.
- **Double press** keeps one continuous iris.
- **Picker** opens in place and both choose and cancel fade the frame.
- **CC pill slider:** centred (x 115-349), 1:1 drag, the track one step up while pressed, an 88 px hit row and 28 px clearance from the bezel.
- **AOD with REC:** a grey `• REC` chip with no red, and a clean entry ramp.
- **CLEAR ALL** flows into the empty state.
- **Reveals** cost nothing.
