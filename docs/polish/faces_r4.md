# Faces: critic round 4

**Build.** `build/nos_sim_crit4_faces.exe` at HEAD 0707127 (includes e1f2215 and fe29043). Evidence is in `build/nos/critic/faces/r4/` (paths below are relative to it); scripts are in `scripts/`, tools in `tools/` (adds `reds.py`, which counts red blobs). COM3 was not touched.

## Verdict
All 10 round-3 items are fixed:
- Recording shows exactly one red on every face, in every state (27 shots).
- AOD shows a grey REC on all 8 faces (1.36–5.09% lit).
- The chip comes up with the wake in 4 steps, and LIFE's hour dot is whole.

`--damagecheck` passes on 25,085 frames across 10 scripts, and there are 0 late frames in 30 runs (max 24.4 ms, bus-bound full repaints).

One new P2 is left: **the REC chip's clock freezes while the face picker is open**. Everything else is P3.

## Round-3 items
| # | Status | Evidence |
|---|---|---|
| 1 Two reds while recording | **Fixed.** Exactly 1 red blob in every rec, rec+unread, bat15 and charging shot. On TAPE that blob is the lamp. | `sheet_rec.png`, `sheet_rec_unread.png`, `tools/reds.py` |
| 2 Chip vanishes on the first press after wake | **Fixed on the device path.** With a recent input (firmware `go_state(PWR_ON)` calls `nos_keep_awake`), the press shows the pressed pill (frames 20–25) with no gap. The round-3 repro was a sim artefact (P3 #3). | `recwake/k4.png`, `recwake/k7.png` |
| 3 No AOD indicator; chip pops on wake | **Fixed.** Grey "• REC" on all 8 faces in AOD. On wake the chip steps 0 / ¼ / ½ / full over frames 1–4. | `sheet_rec_aod.png`, `recwake/k4.png` |
| 4 LIFE backing slices the hour dot | **Fixed.** The 11 px dot is whole. MATRIX's end ghost cells feather 24→16→8 luma instead of being cut. | `crop_life_rec_chg.png`, `crop_r1_rec.png` |
| 5 INVERTED status jammed | **Fixed.** Date 120–131, "⚡50%" 144–158, digits from 171: 12 px gaps on both sides. | `extremes/x6_chg50.png` |
| 6 TAPE AOD date off centre; lamp pops | **Fixed.** The AOD date centres alone (234 / 236 / 228 with the drift) and slides 4 px to 240 over the wake. The lamp grows in the second half of the wake. | `recaod/qw7_*.png`, `aodwake/a7_*.png` |
| 7 Long-press picker has no cancel | **Fixed** (the button). It springs back to the opening face and saves nothing, also mid-drag. The header comment is now correct. | `pkcancel/pk_btn.png`, `sheet_pkcancel.png` |
| 8 Date cross-fade at 15 Hz | **Fixed.** DOT label energy steps 38, 26, 14, 16, 30, 43: one frame per step. | `rollover/m0_020..025` |
| 9 Two idle frames before the Settings pop | **Fixed.** Pill fade on frames 3–6, slide on 7–18. | `fromsettings/fs_choose.png` |
| 10 GLYPH AOD unread dot alone | **Fixed.** The date shows in C_DIS beside the dot. | `crop_glyph_aod_unread.png` |

## Issues (most severe first)
1. **[P2] Picker while recording: the chip's clock is frozen and then jumps; on choose the chip pops in.**
   - What happens: home doesn't tick under the TX_NONE picker, so `H.rec_lbl` holds its last value.
   - Cancel: the chip fades back over frames 4–8 reading "REC 00:11", then jumps to 00:13 on frame 10.
   - Choose LIFE → SEGMENT: frame 7 has no chip, and frame 8 shows the chip at full reading "REC 00:06" (stale by 4 s). Frame 9 reads 00:10.
   - Evidence: `crop_rpk_labels.png`, `recpk/rpk_cancel.png`, `recpk/rpk_choose.png`.
   - Fix:
     - Refresh the label from the picker: export `sys_home_rec_tick()` (it runs `sys_rec_state()` into `H.rec_lbl`) and call it from `f_tick`.
     - In `f_draw`, also draw `sys_home_draw_rec(&sub, 1 - fa)` on `s == F.page` while `F.chosen`, so the chip comes up with the frame-out on the new face.
2. **[P3] The picker's button cancel takes 19 frames (0.63 s), against 6 for a choose.**
   - The spring's tail moves 7 / 4 / 3 / 2 / 1 px over frames 12–16 before the `< 3 px` close starts (`pkcancel/pk_btn_*`).
   - Fix: start the close at `< 12 px`, so the last pixels ride the fade. That saves about 4 frames.
3. **[P3] Sim: `set aod 0` doesn't reset the awake clock.**
   - Any press more than 15.5 s after the last input rebuilds home, and the chip vanishes for 8 frames (`recwake/i0.png`, `rec3/t4.png`).
   - On the device the screen would be in AOD by then, and the wake calls `nos_keep_awake()` (`firmware/main/nos_main.c:249`). That mismatch is what misled round 3's #2.
   - Fix: call `nos_keep_awake()` in `sim/nos_sim.c` when `aod` goes to 0.
4. **[P3] (engine/Settings, seen in the picker flow) Every pop back to Settings lands 1 px right, holds 2 frames, then snaps.**
   - Seen in `fs_choose` frames 18→21 and `fs_btn` frames 32→35. It also happens on a Settings sub-page `pop` (`popsnap/ps_sub`, frames 11→14), but not on the edge-swipe back.
   - The diff is exactly a 1 px x shift (`fs_choose_20_21.png`).
   - Fix: snap the pop's final offset to 0 when `|ox| < 1` on the last TX frame.

## What is already excellent (don't break it)
- **One red, everywhere:** faces yield in 4 steps, LIFE's white now-ring, TAPE's blinking lamp as the REC light, and the grey AOD REC.
- **The chip's feathered backing** (no half-moons), and its press is lit for 3+ frames before the iris.
- **Picker:** in place, with the button cancel springing back to the face it opened on (even mid-drag), and Settings cancel/choose updating the row correctly.
- **Wakes:** even 6-frame ramps on all faces, TAPE's date slide, GLYPH's AOD date, INVERTED's 12/12 spacing, and 30 Hz date cross-fades.
- **Cost:** 0 late frames, and damagecheck PASS on 25,085 frames.
