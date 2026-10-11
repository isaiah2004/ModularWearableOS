# NOS critique: outconn, round 3

Scope: Compass, Barometer, Navigate, Weather (+ wg_weather), Music (+ wg_music), Cellular,
Find phone / Find watch, Recorder, Torch, Call, and the system REC chip these apps feed.
Sim: `build/nos_sim_crit3_oc.exe` (HEAD d634733). Evidence is under
`build/nos/critic/outconn/r3/`. All paths below are relative to it:
- `old/<name>/`: all 36 round-1/2 scripts, re-run.
- `r3_*/`: 14 new scripts (`scripts/r3_*.txt`).
- `z/`: crops.
- `tools/`: `px.py`, `crop.py`, `strip.py`, `dither.py`.

## Verdict

Round 2 landed almost everything:
- Call end and missed call are now clean builds.
- Red is a dot or glyph everywhere: the storm line, Noise, and the call discs.
- Units keep case in every app.
- Navigate's Stop → SAVED and Weather's data swap are real transitions.
- Recorder keeps recording in the background.
- The system REC chip exists.
- Find watch skips whole dots.
- Nav and Recorder copy are fixed.

Performance:
- 0 late frames in 31,902 frames over 50 runs. Worst frame 24.35 ms (bus-bound full repaints).
- `--damagecheck` PASS on 15 scripts (10,261 frames).
- No toast warnings.

What keeps this area off "nothing above P3" is two P2s:
- **#1:** Recorder start/stop is still a one-frame swap of four elements.
- **#2:** the REC chip it feeds puts a second red on the default face.

## Round-2 status

| # | Round-2 issue | Status | Evidence / note |
|---|---|---|---|
| 1 | P1 `[ CALL ENDED ]` over red disc | **Fixed** | Disc folds, then the label fades in (`r3_call/ke_003-008`). Residue: new #11 |
| 2 | P1 No REC indicator outside Nav | **Fixed** (residue) | Face chip and CC header (`r3_nav/n_face_rec.png`, `r3_memo/r_cc_chip.png`). No launcher dot. Two reds on DOT: new #2 |
| 3 | P2 Remote hang-up while ringing | **Fixed** | Both discs dissolve in place, `INCOMING CALL`→`CALL`, `[ MISSED CALL ]` (`r3_call/km.png` f567-f574) |
| 4 | P2 Red as text / fills | **Fixed** | Storm: red dot + white `[ STORM ]`, trend kept (`r3_storm/s_storm.png`, run with `NOS_STORM=1`). Noise: one red meter dot (`r3_memo/r_noise_loud.png`). Call: dark discs with a red X |
| 5 | P2 Units case | **Fixed** | `km/h`, `22 m`, `ALTITUDE 920 m`, `-81 dBm`, `14 s`, `+1.4 hPa / 3 h` |
| 6 | P2 Nav Stop→SAVED hard cuts | **Fixed** | Pill fades f790-f793; header crossfades; SAVED builds (`r3_nav/nh.png`) |
| 7 | P2 Weather swap in one frame | **Fixed** | Out f97-f101, in f101-f105, 17°→18° morph (`r3_wx/ws.png`) |
| 8 | P2 Weather empty state thin | **Fixed** | `No weather yet` + body + `[ UPDATING ]` (`wx_cnone/icon_now.png`) |
| 9 | P2 Find watch half-dots | **Fixed** | Whole dots skipped (`r3_find/fw_ringing.png`) |
| 10 | P2 Music widget press / return / open | **Partly** | Return clean, title travels with the disc (`r3_music/wb`, `wo`). Press is still late: new #5 |
| 11 | P2 Dithered transition discs | **Partly** | Weather/music widgets clean (`dither.py`: ≤ 36 isolated px). Torch is not: new #4 |
| 12 | P2 Recorder stops on leave | **Fixed** | Keeps going; `[ RECORDING ]` toast; chip counts; chip and CC open Recorder (`r3_memo/ml.png`, `r_from_chip.png`) |
| 13 | P3 Quick-tap pressed 2 frames | **Fixed** | Memo and inbox rows hold #313031 for 3-4 frames (`r3_memo/mq_000-002`, `r3_cell/ci_000-003`) |
| 14 | P3 Recorder `00:00` ghost cells | **Fixed** | `--:--` morphs into `00:00` (`r3_memo/r_idle.png`, `mr.png`) |
| 15 | P3 Nav value changes | **Mostly** | Fix arrival 10 frames (333 ms), no blank frame. Stale speed frozen. Residue: new #9 |
| 16 | P3 Level hold feedback / FLAT | **Fixed** | Bezel hold ring f87-f97; readout ramps 2.7°→0.2° then crossfades to `[ FLAT ]` (`r3_compass/cl.png`, `cf.png`). New #10 |
| 17 | P3 Baro polish | **Fixed** | ± on a 4 px dot grid; cancel keeps 1013.3 (`old/r2_baro/k_back_cancel.png`); `TEMPERATURE` + `°C` (`old/baro/b_local.png`) |
| 18 | P3 Buttons outside §8 | **Partly** | CALIBRATE is 200 × 88 at y 372. The icon-disc + caption row is still not in §8 |
| 19 | P3 Find phone stop label swap | **Fixed** | Hint stays at y 370-381 (`r3_find/fs_000-015`) |
| 20 | P3 Music Next blank band | **Fixed** | Title max 255→189→131→65→65→131→189→255 (`r3_music/mx_000-008`) |
| 21 | P3 Weather icons / hourly build | **Fixed** | New crescent and wind (`wx_c8`, `wx_c7`). Hourly is built before it scrolls in (`old/wx_full/w_scroll_flick.png`) |
| 22 | P3 Comet clearance / ON PHONE title | **Fixed** | `RING_R` 222. Title `CALL` + `[ ON PHONE ]` under the timer |
| 23 | P3 Torch red gauge contrast | **Fixed** | Black on a #9A0000 pill (`r3_torch/t_red_level.png`) |
| 24 | P3 Copy and spacing | **Mostly** | Widow fixed. `TAP RADIO TO TURN ON` in caps. `LTE / HOME` → RADIO gap is 12 px, not 16-20 (`r3_cell/c_live.png`: y 304 → 316) |
| 25 | P3 Toast over title / AA cast | **Partly** | The toast covers the title after 1 frame (`z/mns_toast.png`). The colour cast is now magenta: new #6 |

## Issues (most severe first)

1. **[P2] Recorder: start and stop are hard cuts.** At stop, f447 → f448, one frame changes four things:
   - title `RECORDING` → `RECORDER`;
   - hint `TAP TO STOP` (#9C999C) → `[ SAVED ]` (white);
   - the waveform goes from white to grey;
   - the page dots pop in.

   Start does the reverse in the release frame (f40). Only the button morphs.
   Evidence: `z/ms_0_5.png` (`r3_memo/ms_000-005`), `r3_memo/mr.png`.
   Fix: run all four on the button's ~150 ms clock:
   - 4-step colour ladder on the title and the wave;
   - the hint out over 2 frames, then `[ SAVED ]` in over 2;
   - the page dots fade in over 4 frames, as the home dots do.
2. **[P2] REC chip: two reds on the default face.** DOT's red seconds dot (#D51820 at 233,233) blinks
   alongside the chip's red dot at (165,421), for both a memo and a track.
   Evidence: `r3_memo/ml_022.png`, `r_chip_pressed.png`, `r3_nav/n_face_rec.png`.
   Fix (owner: shell/faces): while `sys_rec_state()` ≠ 0, face_dot draws its seconds dot in
   `C_PRI`. Faces that keep red seconds take the same rule.
3. **[P3] Noise: meter and hero disagree for over 0.67 s.** After `set db 92` the meter reaches the red dot in
   7 frames, but `49` / `MODERATE` / `Normal conversation` stay for all 20 filmed frames
   (`r3_memo/mn.png`). Fix: when the level crosses a band (85 dB), update the hero and label
   at once instead of waiting for the ~1 Hz tick.
4. **[P3] Torch entry is a full-disc Bayer dither for 3 frames** (f3-f5): 62,514 / 21,884 / 8,494
   isolated pixels (`z/te2.png`, `r3_torch/te.png`). On purpose (`con_torch.c:5`), but it breaks
   §2.4 / r2 #11. Fix: a 4-level flat crossfade (#404040 → #808080 → #C0C0C0 → white), or document
   the exemption in §8.
5. **[P3] Music widget: the pressed fill appears on frame 6 of a hold** (167 ms). List rows take 2 frames
   (`r3_wgpress/hp_000-005`: white ×5, then #9C999C).
6. **[P3] `C_PRI` 0xEF5D renders #EEEAEE (G −4), a pink white.** It shows on the level bubble
   next to the pure-white ring (`r3_compass/cf_000` vs `cf_010`), on chip text and on reveal dims.
   Use 0xEF7D (palette owner).
7. **[P3] REC chips show no pressed fill on a 2-frame hold** (face and CC: `r3_memo/r_chip_pressed.png`,
   `r_cc_chip_pressed.png`, no #181818 behind the chip).
8. **[P3] The Recorder hero says `00:13` at save, but the list says `14 s`.** `fmt_len` rounds and the hero
   floors (`r3_memo/ms_005`, `mq_000`).
9. **[P3] Navigate.**
   - On refix, `[ NO FIX ] 0:12 AGO` flips to `0:00 AGO` while it fades out (`z/refix.png`).
   - `[ SEARCHING 14 SATS ]` → `[ SEARCHING 11/14 ]` swaps text and width in one frame (`r3_nav/nf_013→014`).
   - The fix arrival is 333 ms; trim it to ≤ 300 ms.
10. **[P3] Level entry scales the bubble up** from ~3 to ~22 px over f102-f111 (`r3_compass/cl.png`).
    §8 says no scale-ups; dot-build it at full size.
11. **[P3] Call end leaves a dark-red residue** (#391818, 4 px) between `CALL` and `ENDED` for 1 frame
    (`z/en7.png`, `old/r2_end/en_007`). Stop drawing the X once r < 6 px.
12. **[P3] Storm stack is cramped.** Gaps from the title to `[ STORM ]` to `FALLING …` are 13 px and 7 px
    (`r3_storm/s_storm.png`). Use 16 / 16, or put `[ STORM ]` under the hero unit.
13. **[P3] Weather/Cell residue.**
    - The `LTE / HOME` gap is 12 px (r2 #24).
    - Add the icon-disc row to §8 (r2 #18).

## Already excellent (don't break)

- **Performance.** 0 late frames over 31,902 frames. Damage-tracked output equals full repaint on all new
  morphs.
- **Call.**
  - Accept is a true morph.
  - Missed call dissolves both discs in place.
  - End folds the disc, then the label comes up.
  - A decline over Music reveals Music without a rebuild.
- **Navigate.**
  - Hold-to-stop ring, faded pill, built SAVED summary.
  - Frozen dim values under `[ NO FIX ]`.
  - White REC dot on the track page.
- **Recorder.** Background service with a `[ RECORDING ]` toast. The chip counts and opens the recorder.
  `--:--` → `00:00` morph.
- **Weather.** Full crossfade-and-morph data swap, a proper empty state, hourly pre-built.
- **Find.** Whole-dot knockout around the text; the hint never moves.
- **Units, red discipline, absent and phone-absent states** are consistent across all ten apps.
