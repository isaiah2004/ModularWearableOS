# Faces: critic round 3

Scope: the 8 faces (DOT, MATRIX, GLYPH, SPLIT, LIFE, SEGMENT, INVERTED, TAPE) in every state: idle, minute/hour/midnight
rollovers, 12 h, AOD entry (host path: `home_now` + `set aod 1`, from home and from an app), wake (also at a minute change),
unread, low battery, charging, complication presses, the REC chip (memo recording), and the face picker (long press and
Settings: swipe, choose by tap/button, cancel).

**Build.** `build/nos_sim_crit3_faces.exe` at HEAD 65754b3, which includes d00442b (the 8 px unread dot, red only where the face
has no other red). All runs were re-done on that build. Evidence is under `build/nos/critic/faces/r3/`, and paths below are
relative to it. Scripts are in `scripts/` and helpers in `tools/` (`fdiff.py`, `maxd.py`, `inkx.py`, `aodstats.py`, `sheet.py`).
Frame numbers are the `NNN` in `name_NNN.png`. COM3 was not touched.

**Not re-filmed.** The sleep → tap build-in: the sim's virtual clock never triggers home's `woke` test, so the `buildin/` films
show no build. The build-in code is unchanged since round 2, except SEGMENT's.

## Verdict

Round 2 landed completely: 18 of 19 items are fixed and #19 (picker on INVERTED) is accepted as designed. Every wake is now an
even 6-frame ramp:
- SEGMENT's worst wake frame is down from 70% to 30% of the final light.
- MATRIX peaks at 23% and GLYPH at 14%.
- The other moments that were rough in round 2 are clean: stale picker faces, the GLYPH date window, TAPE's midnight rewind,
  INVERTED's minute change while it opens, 12 h trims, and the date cross-fades.

The engineering is clean too: `--damagecheck` passes on 11,536 frames across 4 scripts, there are 0 late frames in 21 runs
(max 24.4 ms, all bus-bound full repaints), and AOD is 1.25–5.17% lit with APL ≤ 1.12%.

What keeps this round above the P3 bar is the new **REC chip**, which the faces don't yet know about:
- While a memo records, every face shows two reds.
- The chip vanishes on the first press after a wake.
- AOD shows no sign that the mic is on.
- On LIFE the chip's backing slices the "now hour" dot.

At rest, outside recording, all 8 faces would ship.

## Round-2 items

| # | Item | Status | Evidence (r3) |
|---|------|--------|---------------|
| 1 | Picker from Settings brightens, then hard-cuts | **Fixed.** The face stays dimmed and the pill fades over frames 3–6, then the reverse TX_PUSH slide runs over frames 9–15. 2 idle frames remain (P3 #9). | `fromsettings/fs_choose.png` |
| 2 | SEGMENT wake: 70% in the last frame | **Fixed.** The last three frames are 26.0 / 27.0 / 29.8%. | `aodwake/w5.png`, `tools/fdiff.py` |
| 3 | GLYPH wake pops | **Fixed.** The date and second dot grow from frame 3, and the unread dot stays at the window. | `aodwake/w2.png` |
| 4 | MATRIX ring pops | **Fixed.** Steps are 4.8 / 15.5 / 22.0 / 16.2 / 22.8%. | `aodwake/w1.png` |
| 5 | Picker replays stale morphs | **Fixed.** INVERTED arrives at 10:31 and SPLIT at 12:05, with no roll. | `stale/st_inv.png`, `stale/st_split.png` |
| 6 | INVERTED hard-cuts at a minute change while opening | **Fixed.** 10:15 → 10:16 morphs inside the disc over frames 7–10. | `aodwake/am6.png` |
| 7 | GLYPH date jumps between windows | **Fixed.** A 4-step cross-fade, 3 → 9 o'clock, over frames 30–35. | `glyphslot/g_slot.png` |
| 8 | TAPE midnight swap | **Fixed.** The rewind runs from frame 21 and the date cross-fades over frames 22–26. | `rollover/m7.png` |
| 9 | 12 h pads and centring | **Fixed.** LIFE and SEGMENT show "9:41". TAPE's ink centre is at 232.5, and DOT's lone hour is centred at 232.5. | `sheet_h12_0941.png` |
| 10 | Wake rhythm 1-2-1-1 | **Fixed.** 5 changing steps with no held frame on all 8 faces. | `aodwake/w*.png` |
| 11 | SPLIT unread goes grey → white → red | **Fixed.** At (233,24) it goes (164,129,131) → (213,24,32) over frames 0–5. | `unread_aod/uw3_*.png` |
| 12 | SPLIT pressed rect is a fixed width | **Fixed.** The rect hugs "HR 68". | `taps/t3hr.png` |
| 13 | SEGMENT ring has no pressed state | **Fixed.** A 60 px rounded tile lights under the segments. | `crop_seg_ringpress.png` |
| 14 | AOD low battery on SPLIT only | **Fixed.** All 8 faces show "4%" (+0.07–0.12% lit). | `sheet_bat4_aod.png` |
| 15 | LIFE unread pairs with the ring dot | **Fixed.** It is now at y 64. | `sheet_unread.png` |
| 16 | Date hard-cuts at midnight | **Fixed.** A 5-step cross-fade on every face (cadence is P3 #8). | `rollover/m*_014..029` |
| 17 | LIFE breath asymmetric | **Fixed.** Holds are 4-4-4-3-2-4-4 on both slopes, with 9 frames at each end. | `seconds/s4_*.png` |
| 18 | SEGMENT values left-heavy | **Fixed.** Values centre at 228 and labels at 236; the group centres at 232 and each column is centred. | `rest/f5_1009.png` |
| 19 | Picker: INVERTED grey disc, DOT pill | **Fixed / accepted.** The pill (y 396–430) clears DOT's digits. INVERTED's 0x8C grey disc stays, as designed. | `sheet_picker.png` |

## Issues (most severe first)

1. **[P1] REC chip + face: two reds on every face while recording.**
   §2.2 allows one red per screen. With a memo running:
   - DOT, GLYPH, INVERTED, LIFE and TAPE show their own red as well: the second dot, the now-dot or the lamp.
   - MATRIX, SEGMENT and SPLIT show the red unread dot as well. d00442b only checks the face's own red, not the chip's.

   Evidence: `sheet_rec.png`, `rec/r*_rec.png`, `rec/r*_rec_unread.png`.

   Fix:
   - Add `int rec` to `face_ctx`, set from `sys_rec_state() != 0` in `build_ctx`.
   - While it is set, faces draw their red in C_PRI (C_INK on INVERTED). LIFE's now-dot becomes a C_PRI ring (23 px, 5 px C_BG
     core) so it doesn't merge with the past dots.
   - The unread dot rests at C_PRI.
   - On TAPE, the lamp *is* the REC light: drop the chip's dot there and keep the lamp red.

2. **[P2] The REC chip vanishes on the first press after an AOD wake.**
   - TAPE (`rec4/v7.png`): touch at frame 3, chip at 0 red px in frames 3–4, a 1/4 fade in frame 5, pressed in frame 6.
   - LIFE (`rec3/t4.png`, frames 3–6): the same.
   - DOT pressed later shows no gap (`rec3/t0.png`).

   Likely cause: the first touch resets `nos_awake_ms`, so home's `woke` test fires a build-in (`H.c.enter = 0`). The faces skip
   it via `fk_wake_skip`, but `draw_rec` multiplies by `enter`.

   Fix:
   - Set `H.prev_awake_ms = 0` on the AOD → awake edge.
   - In `draw_rec`, drop the `enter` factor when the face skipped its build.

3. **[P2] AOD has no recording indicator, and the chip pops in at full on wake.**
   - In AOD (`rec2/q_aod.png`, `rec/r0_rec_aod.png`) there are 0 chip pixels while the mic records.
   - On wake, `rec/rw0_000` already shows the chip at full ink while the face is at k = 1/6.

   Fix:
   - In AOD, draw "REC" in C_DIS with a 6 px C_SEC dot (no red, no backing) at the face's slot plus the AOD drift; about 0.1%
     lit.
   - On wake, step the chip with the face's wake k (4 steps).

4. **[P2] LIFE: the chip's backing slices the day ring's current-hour dot.**
   When the status shares the line (charging or ≤ 20%), the backing x 114–349, y 387–417 cuts the 11 px white hour-10 dot at
   (339,417) into a half-moon, and clips the dim dot on the left. Evidence: `crop_life_rec_chg.png` (`rec/r4_rec_chg.png`).
   MATRIX's ghost cells at the pill ends are sliced the same way (`crop_r1_rec.png`, P3).

   Fix: feather the backing like the picker moat (2 px solid + 4 px feather, 8 steps), and on LIFE clip it to r ≤ 200 (the ring
   is at 212).

5. **[P3] INVERTED: the battery line is jammed onto the time.** The date ink is at y 128–139, "⚡50%" at 152–165 and the digits
   start at 172, so the gap below the battery is 6 px against 12 above (`crop_inv.png`, `extremes/x6_chg50.png`). Fix: date
   baseline 132 and status 156, or put the status below the steps at y 368.
6. **[P3] TAPE: the date is off centre in AOD and the lamp pops on wake.** The ink centre is 242 in AOD and in wake frames 0–4,
   against 231.5 awake; the 9 px lamp appears whole at frame 5 (`unread_aod/uw7.png`). Fix: centre the date alone in AOD, slide
   it 8 px with k, and grow the lamp from k = 0.5.
7. **[P3] The long-press picker has no cancel.** An edge flick from x = 5 only rubber-bands (`picker2/pk_cancel.png`): the
   engine excludes TX_DISSOLVE screens from back, yet the sys_faces.c header still says "Left-edge swipe cancels". From
   Settings the edge swipe does cancel. Fix: pop with TX_DISSOLVE without saving on a left-edge drag, or correct the comment.
8. **[P3] The date cross-fade runs at 15 Hz.** Each of its 5 steps holds 2 frames (DOT label energy
   32,32,21,21,9,9,10,10,25,25,37), next to 30 Hz time morphs. Fix: `XF_S = 5/30` s.
9. **[P3] Picker from Settings: 2 idle frames before the pop** (`fs_choose` frames 6–8). Fix: call `finish()` at 0.12 s.
10. **[P3] GLYPH AOD: the unread dot sits alone** at about (292,233), beside an empty date window, and reads as a stray tick
    (`unread_aod/u2_aod.png`). Fix: draw the date in C_DIS in AOD (+0.2% lit).

## What is already excellent (don't break it)

- **Wakes:** every face morphs from its AOD layout in 6 even counted frames. No frame adds more than 30% of the final light,
  except TAPE's low-energy reels. INVERTED's opening, with the minute morph inside the disc, is the best moment in the set.
- **Rollovers:** GLYPH's window cross-fade, TAPE's rewind, MATRIX's drain on the beat, LIFE's hour drain, and SPLIT's
  odometer on the changed digits only.
- **Stale faces settle** in the picker instead of replaying changes. The minute morph also runs inside the picker
  (`picker2/pk_min.png`).
- **States:** 12 h is right on all 8 faces. Battery and charging show on all 8, awake and in AOD. The unread dot is 8 px and
  colour-correct when nothing records.
- **Presses:** SPLIT rects hug the value, SEGMENT's ring tile, and the iris opens from the finger.
- **Cost:** 0 late frames, damagecheck PASS on 11,536 frames, idle frames repaint only the seconds element, and AOD is ≤ 5.2%
  lit.
