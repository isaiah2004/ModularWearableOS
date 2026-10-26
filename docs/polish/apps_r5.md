# apps (healthtime + outconn): critic round 5

Sim `build/nos_sim_crit5_apps.exe` at 899af0a. Evidence is under `build/nos/critic/r5_apps/`, and all paths below are relative to it. `old/` holds all 109 earlier scripts, re-run. `new/` + `scripts/` hold 45 new scripts. `z_*.png` are montages. COM3 was not touched.

## Verdict
**Not met.** All 9 round-4 items are fixed, and the ring double-tap P1 is fixed under a real 07:00 alarm. But the new engine rule "a tap during an open lands it" sends that tap through to the screen that is still sliding in. That is a **P1** in three apps. Two P2s and three P3s follow. Cost: 154,905 frames, 0 late, worst 24.4 ms (bus-bound full repaint). `--damagecheck` 63/63 PASS (`dc_results.txt`). Three old scripts exit 1 from their own stale lines, not app faults: `r4_stuck` expects the old bug, `baro_back` has a stale expect, and `wc` opens a renamed screen.

## Round-4 status
| # | Item | Status | Evidence |
|---|---|---|---|
| HT1 | Ring 2nd tap grabs the drop | **Fixed.** Real alarm at 07:00: drop f204-210, face at f211. 12 variants all leave cleanly: gaps 1-12 frames, slide, back, button, held touch. TIME'S UP STOP×2 drops f557-563. 5 STOP/+1/button cross-inputs: the first one wins | `old/healthtime_r4_stuck/db.png`, `new/ring_snz_*/f.png`, `old/healthtime_r4_tdbl/td.png`, `z_tr_end.png` |
| HT2 | Reveal tints green/magenta | **Fixed.** Grey steps #000→#101010→#181818 | `old/healthtime_r4_tplus/pl_009`, `old/healthtime_r4_back/wb_004` |
| HT3 | Confirm-delete cut | **Fixed.** C_SURF2 press inside the red outline f131-134, then the editor slides off unchanged f135-140 | `old/healthtime_r4_delcatch/dl.png` |
| OC1 | Recorder under call/Find | **Fixed.** Keeps RECORDING, 00:02→00:06. No toast over INCOMING CALL | `old/outconn_r4_r4_recover/rc.png`, `rc_toast_end.png` |
| OC2 | Phone/module loss cuts | **Fixed.** Half ink → off → build, both ways: Baro, Nav, Compass, Cell, Recorder (also mid-recording), Music, Find. Residue: #3-#5 | `new/up_*/{off,on}.png`, `new/ph_*/{off,on}.png` |
| OC3 | Music ring/times jump | **Fixed.** Prev drains over f365-369 and times swap with the title (f368). Reconnect sweeps 0→1:13 over f218-226, no "Unknown track" | `old/outconn_r4_r4_live/mpf.png`, `mon.png` |
| OC4 | Noise in-between band | **Fixed.** MODERATE out f788-789, HEARING RISK in f790-791 | `old/outconn_r4_r3_memo/mn.png` |
| OC5 | Torch red gauge | **Fixed.** Unlit dots #620000 on #9C0000 | `z_torch.png` |
| OC6 | §8 icon-disc row | **Fixed** (c206ba3). Cellular matches it (169/297). Call does not: #6 | `old/outconn_r4_r3_cell/c_live.png` |

## New issues
1. **[P1] Double tap on a control that pushes: the 2nd tap lands the push and acts on the new screen** (regression from d4e7812).
   - In `press_begin`, `if (T.state == TXS_AUTO) tx_finish(); send(IN_DOWN);` jumps the screen in (~140 px in one frame) and delivers the tap.
   - World clock EDIT CITIES ×2 removes New York, 4/4 → 3/4. Jump at f85→f86 (`new/dt_wc/f.png`).
   - Timer CUSTOM ×2 starts a 10 MIN timer the user never saw. Same result at every gap from 0 to 11 frames (`new/dt_timer/f.png` f52→f53, `z_dtg.png`).
   - Cellular SMS ×2 opens and reads the first message (`z_dt.png`).
   - Fix (`nos.c` ~l.525), before the `tx_finish` line: `if (T.state == TXS_AUTO && T.use_tween && !T.popping) { P.owner = 3; return; }`. This spends the touch and lets the ≤0.34 s tween land by itself, as closes already do. Fix the comment at l.106 to match.
2. **[P2] Alarm: after slide-to-stop commits, a button press or SNOOZE tap snoozes it, and it rings again at 07:09.**
   - The knob hits the end with FB_SUCCESS (f212). `ring_input` only gates on `snz_fire`/`leaving`, and `R.committed` waits about 5 frames for the knob spring to settle.
   - Evidence: `new/ring_stop_btn/f.png` (`[ SNOOZED ]` at f218). `z_stop9.png` shows ALARM back at 9 min for btn and snz, and the face for slide only.
   - Fix (`time_alarm.c:853`): `if (R.snz_fire > 0 || R.leaving || R.committed) return 1;`
3. **[P2] Heart rate does not come back when the HR strap is plugged in while the app is open.**
   - It stays "Not connected" 10 s after `present 0x1FF`. Re-opening shows MEASURING (`z_hr_replug.png`).
   - Cause: `heart_tick` polls only while `H.present`. Every other module app recovers live.
   - Fix (`fit_heart.c:550`): `else if (hal_ext_present() & EXT_HR) { hal_hr_enable(1); H.present = 1; set_mode(M_MEASURING); }`
4. **[P3] Barometer and Navigate titles blink out during the module swap.** The title is the same in both states, but its C_SEC steps 157→82→0→157 (f77-79 and f122-124). Cellular and Recorder keep their titles steady (`new/up_baro/off.png`, `new/up_nav/on.png`). Fix: draw `ui_title` outside the 2-step dim, as `con_cell` does.
5. **[P3] Music reconnect flashes "Nothing playing".** It builds f207-213 and leaves f214-215, before Kerala arrives at f216 (~0.3 s, `old/outconn_r4_r4_live/mon.png`). Fix: after a phone connect, hold the title-only page up to 1.0 s for musicinfo, and build the empty state only if none arrives.
6. **[P3] Call discs are off the §8 icon-disc standard.**
   - Measured: r 44 at x 150.5 / 314.5 (164 apart), y 325.5, caption 14 px under (y 384).
   - §8 says x 169 / 297, y 360, caption 16 px under (`old/outconn_r4_r4_recover/rc_toast_end.png`).
   - Fix: add an exemption to the §8 row: "Call: x 151/315, y 326, wider gap so accept and decline are hard to mis-hit". Or move the discs to the standard.

## Already excellent (keep)
- Ring exits: constant-speed 7-frame drops that nothing can grab back. The reveal lands on the covered app (Stopwatch). Snooze and stop are honoured exactly.
- Module and phone swaps: one shared out→build. Compass's dial build-in. Heart keeps its icon through the unplug.
- Recorder under a call, Music ring tweens, the noise band hand-off, the readable red torch gauge.
- Pushes now run 10-frame monotonic tweens (`old/outconn_r4_cell/c_inbox_push.png`). Zero late frames, damage-check clean.
