# Shell + faces, critic round 5 (final verification)

Sim `build/nos_sim_crit5_sf.exe` at 899af0a. Evidence is under `build/nos/critic/r5_shellfaces/`: all 42 shell-r4 and 31 faces-r4 scripts (`f_*`), plus `r5_lite`, `r5_rim` and `r5_power`. Tools are in `tools/` (`edge band ring shift hist crop mosaic`). Results: 77 runs, 82,421 frames, **0 late** (max 24.4 ms). `--damagecheck` **PASSES on 77 of 77** (`dc/results.txt`). COM3 was not touched.

## Verdict
**The bar is met: nothing is above P3.** All 13 round-4 items are fixed, except the cross-team music glyph (P3, still open). The faces' P2 is gone: the REC clock now ticks under the picker, and the chip fades in with the new face. The spring creeps are gone too: push, power rise, picker cancel and gap close all run on tweens. Three new P3 nits came up, all in hand-offs.

## Round-4 status: shell
| # | Status | Evidence |
|---|---|---|
| 1 Non-finger spring creep | **Fixed.** Push: OPEN edge 324→84 in 9 frames (-90 … -6, -2), idle at f224. Gap close: 116, 107, 102, 100. Power rise: 344, 333, 330, 329, 328. Picker cancel: 8 frames to home (was 15). | `r4_push/pt,pd`, `r5_power/pw`, `r4_picker/pc` |
| 2 Slow-drag press flash | **Fixed.** C_LINE2-band px stay flat on every slow-drag film (sc 987, sw 714, ls 452, pw 553; was 10k+ flashes). | `r3_pressflash/*` |
| 3 Double press list → cut | **Fixed.** The list dims for 1-2 frames, then the app builds inside the same iris (f144→f145; slow press f146-147→f148). | `r2_double/dbl`, `r4_dbl/dbs` |
| 4 CC note hard-cuts | **Fixed.** A 3-frame crossfade in (tap st_003-005, slider sl_006-008) and about 6 frames back. | `cc_st.png`, `cc_st2.png`, `cc_sl.png` |
| 5 Magenta dim | **Fixed.** Tinted neutrals peak at 1.2-4.9k AA px per frame (was 30-47k). | `r4_push/pt_003` |
| 6 Picker rim jumps | **Fixed.** 0, 16, 41, 57, 82, 98, 115, 139, 157: no step above 25. | `r5_rim/ro` |
| 7 AOD REC blinks out on wake | **Fixed** (no gap). There is a new overlap residue: #1 below. | `awr.png` |
| 8 Last app | **Fixed.** Heart rate opened from its widget, and a double press reopens HEART RATE (f287). | `r4_last/frames.csv` |
| 9 Music glyph swap (con_music.c) | **Open (P3).** Still play at f104, then an empty disc at f105, then pause builds over f106-108. | `r4_music/mu.png` |

## Round-4 status: faces
| # | Status | Evidence |
|---|---|---|
| 1 REC clock frozen under the picker (P2) | **Fixed.** On cancel the chip fades in reading 00:13 and ticks to 00:14 at f514. On choose the chip fades in with the frame-out at the live 00:10. | `f_recpk/rpk_cancel,rpk_choose` |
| 2 Button cancel takes 19 frames | **Fixed.** 9 frames to home (f224-232): the slide runs 22 … 13, 7, 2, then the fade. | `f_pkcancel/pk_btn` |
| 3 `set aod 0` keeps the stale awake clock | **Fixed** (k4/k7 show no gap). A 2-frame vanish is left only in the sim-only "17 s idle, no AOD" case (`f_recwake/i0` f1300-01); this is not a device path. | `f_recwake/k4,k7,i0` |
| 4 Pop to Settings snaps 1 px | **Fixed.** The pops end 21, 21, 21, 21, 13, then 0, with no hold or snap. | `tools/shift.py` on `fs_choose`, `fs_btn`, `ps_sub` |

## New issues
1. **[P3] AOD → wake while recording: the two REC labels overlap for 5 frames.**
   - What happens: the AOD "• REC" is centred (dot x≈210, text x 222-261), but the awake chip sits at dot x≈167, text x 180-218, timer x 233-303. The crossfade in `draw_rec_a` (sys_home.c:654-655) draws both, so frames 1-4 read "RE€RB00:04", and a ghost "R" still shows at frame 4.
   - Where: every face (DOT `awr.png`, LIFE `awk4.png`, GLYPH `awk7.png`).
   - Fix: in the hand-off, draw the AOD label at `x = lerp(aod_x0, x0, EASE_STANDARD(kw))`. Give `draw_rec_aod_a` an x override. Its "• REC" then glides about 42 px onto the awake chip's "• REC" while the backing and timer fade in.
2. **[P3] Picker lite draw: ghost detail cuts on a button cancel. Open and choose fade it in 2 steps.**
   - What happens: on the cancel's first slide frame, MATRIX's grey-41 ring (5.8k px) drops to 0 (`pk_btn` f223→f224, `pk_slide.png`). At the end, DOT's ghost cells (16-24 luma, about 17k px) pop in on one frame (pc f113→f114, `pc_ghost.png`, `tools/hist.py`). Open fades 16→8→0, and choose fades back in 2 steps (`lc` f277-279).
   - Cause: `F.c.lite = sliding ? 1 : dim_lin()` (sys_faces.c:135), and the cancel slides until its last frame.
   - Fix: while `F.cancel`, use `F.c.lite = fmaxf(dim_lin(), q8(1 - mtw_progress(&F.cancel_tw)))` instead of the `sliding` override. Ghosts then return in the same 2 steps as choose, and the ring dims instead of cutting.
3. **[P3] Settings → picker → button: one idle frame between the fade-out and the pop.**
   - What happens: f1119 is identical to f1118, then the pop runs f1120-1126. The cancel takes 0.57 s in total.
   - Evidence: `f_pkcancel/fs_btn`, frame 13 of `shift.py`.
   - Fix: start `nos_pop()` on the frame the frame-fade reaches 0, not the frame after (`f_tick`: run `finish()` when `nos_t - F.t_chosen >= close_s() - 1/NOS_FPS`).

## Already excellent (don't break)
- Every non-finger motion is a tween with a ≤3 px tail. Every pop runs at constant speed and lands with no snap.
- The double press is one continuous iris with a dim-out. The CC note crossfades both ways.
- Recording: one red everywhere, the picker keeps a live REC clock, and the chip fades with the frame.
- Picker: in place, the rim ramps evenly, lite draw on open and choose, 9-frame cancel, and finger slides keep their detail.
- 0 late frames and 77/77 damagecheck PASS.
