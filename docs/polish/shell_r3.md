# Shell critique, round 3

Area:
- home: face page, widget pager, unread dot, REC chip;
- control centre (CC);
- notifications: list, detail, banner;
- launcher, power sheet, boot, AOD entry and wake, toasts;
- the engine transitions as seen from the shell: iris, dissolve, push, rise/drop, back swipe, button home, double press.

Sim: `build/nos_sim_crit3_shell.exe`, rebuilt at 65754b3, so it includes d00442b and 6e4575e. Every image cited
is under `build/nos/critic/shell/r3/`, one folder per script, and paths below are relative to it.
- Scripts are in `r3/scripts/`: all round-1/2 scripts plus `r3_btnclose`, `r3_toast`, `r3_toast2`,
  `r3_power`, `r3_pressflash`, `r3_aod`, `r3_rec`, `r3_boot`.
- Frame numbers are the `fNNN` labels in the contact sheets.
- The board and COM3 were not touched.

## Verdict

Round 2's fixes landed almost across the board: 26 of 31 are fixed, 1 is accepted as it is, 2 are partly fixed, 1 is not
fixed and 1 has regressed.
- Back to the launcher is a zero-cost reveal.
- Widget pages build as they slide in.
- Button closes leave from where the sheet is, with no tail.
- Toasts fit the glass and wait for banners.
- The power sheet is on the §8 grid, with a proper arm.
- The iris never shows a black frame.
- AOD dims in and out in 3 + 3 frames.
- The notification list meets every §8 card, empty-state and button rule.

Nothing is P0 or P1 any more. What stands between the shell and "nothing above P3" is four P2s:
- two reds on the face while a recording runs;
- a double press that opens, reverses and re-opens;
- pressed flashes on slow-starting drags;
- a banner that brakes into the bezel.

Performance is clean: 0 late frames across all 36 runs (about 14,000 frames). p95 reaches 24.4 ms only in
full-screen motion, which is bus-bound. `--damagecheck` passes on 16 scripts.

## Round-2 status

| # | Round-2 issue | Status | Evidence |
|---|---|---|---|
| 1 | P0 launcher black frame + rebuild on back | **Fixed.** f174 is the last app frame; f175+ are idle at 1.5 ms with no rebuild. | `r2_tail/back.png` |
| 2 | P1 widget pages rebuild after sliding in | **Fixed.** Timer and Music build as they slide in (f145-151, f197-201); nothing blanks at settle. The weather city swap is a one-time sim data arrival and now crossfades (f98-103). Residue: #12. | `r2_pages/pg3.png`, `pg5.png`, `pg_back.png` |
| 3 | P1 button mid-drag snaps the sheet open | **Fixed.** The hairline goes 79 → 49 → gone; it never opens. | `r2_home_mid/dp.png` |
| 4 | P1 toasts clipped and over banners | **Fixed.** `[ SAVED ]` sits in x 151-314 against a 119-347 chord. Long copy drops (y 50/62) and still fits. A toast waits for the banner (shows at f200, after the banner is out at f187). | `r3_toast/t_*.png`, `r3_toast2/tq2.png` |
| 5 | P1 restart one-tap, label cuts | **Fixed.** Arms with "TAP AGAIN TO RESTART" and 3 dots in a 320 pill, grown over 4 frames. The commit crossfades. Residue: #9. | `r3_power/arm_r.png`, `com_o.png` |
| 6 | P2 power geometry | **Fixed.** Hero at pitch 9; SLEEP/RESTART 140x88 at x 154/312, y 272; POWER OFF 200x88 at y 372. | `r3_power/pw_rest.png` |
| 7 | P2 pressed flash at scroll start | **Partly fixed.** Fast flicks are clean (`launcher/ln_fling.png`), and Settings under a back swipe is clean. Slow-starting drags still flash (new #3). | `r3_pressflash/*.png` |
| 8 | P2 pressed only 2 frames on a quick tap | **Fixed.** CLEAR ALL is pressed f265-267. CC toggles morph from the pressed level at once. | `notifs3/nt_clear.png`, `cc_tap_timing/cc_note.png` |
| 9 | P2 armed disc on a cancelled swipe | **Fixed.** The disc stays C_SURF2 through the spring-back (f313-317). | `r3_pressflash/cs.png` |
| 10 | P2 banner stays over the CC | **Fixed.** It flies up as the CC drops (f239-241). | `banner/bn_pull.png` |
| 11 | P2 cards r30 C_SURF | **Fixed.** r40, (24,24,24) = C_SURF2. | `notifs/nt_list_settled.png` |
| 12 | P2 count numeral illegible | **Fixed.** 5x7 at pitch 3, 21 px tall. | same |
| 13 | P2 empty state custom | **Fixed.** `ui_absent` plus `[ ALL CAUGHT UP ]`. | `notifs3/nt_empty2.png` |
| 14 | P2 detail buttons 68 tall | **Fixed.** OPEN/DISMISS pair, 140x88 at y 372. | `r3_btnclose/detb.png` |
| 15 | P2 long-press rim drops out | **Fixed.** Rim levels 16, 41, 57, 74, 98, 115, 139, 156: monotonic, with no gap at the hand-off (f39/40). | `faces_power/fp_open_*.png` |
| 16 | P2 13-frame button-close tail | **Fixed.** CC, notifications, power, detail and app iris all leave in 7 frames. Residue: #5. | `r3_btnclose/*.png` |
| 17 | P2 picker leaves sideways on back | **Fixed.** The edge swipe pages the picker instead (SPLIT → GLYPH). | `faces_power/fp_cancel.png` |
| 18 | P2 iris deletes the icon / black frames | **Fixed.** The seed sits on the tapped icon at f301; the app shows from f302. | `launcher/seed_zoom.png` |
| 19 | P3 AOD entry 1-frame cut | **Fixed.** From the face: dim 3 frames (f26-28), then AOD up 3 (f30-32). From a widget or app: up in 4. Wake brightens in 5. | `r3_aod/af.png`, `awg.png`, `aap.png`, `aw.png` |
| 20 | P3 Torch looks like a toggle | **Fixed.** Outlined nav tile, like Settings. | `control/cc_open.png` |
| 21 | P3 slider 10 px left | **Fixed.** Ink x 101-361, centre 231. | same |
| 22 | P3 home page dots custom | **Accepted.** Now at `UI_DOTS_Y` 440. The custom drawer carries the capsule and the white-face ink dots, which `ui_page_dots` can't. List it as exempt in §8. | `home/page1.png` |
| 23 | P3 launcher "APPS" header | **Fixed.** `ui_title` at y 70, lifting away with the scroll. | `launcher/ln_rest.png` |
| 24 | P3 pressed launcher disc C_SEC | **Fixed.** `lut_press` lifts to C_LINE2 (f406). | `r3_pressflash/ls.png` |
| 25 | P3 banner tap has no pressed state | **Not fixed.** The card leaves at f450 with no lit frame. | `banner/bn_tap.png` |
| 26 | P3 double press shows the launcher | **Regressed** to an open-reverse-reopen (new #2). | `r2_double/dbl.png` |
| 27 | P3 sim `set aod` animates | **Fixed.** `home_now` mirrors the host. | `r3_aod/*` |
| 28 | P3 picker keyline crescents | **Fixed.** A 6 px feathered moat; DOT's bottom row is whole. | `faces_power/fp_rest.png` |
| 29 | P3 green text AA fringe | **Partly fixed.** About 250 px per frame are still 2-3 G steps high (new #11). | `r2_reveal/nt_list.png` |
| 30 | P3 CLEAR ALL pill vanishes | **Fixed.** It leaves last in the stagger (f268-274). | `notifs3/nt_clear.png` |
| 31 | P3 toggle ramp dips | **Fixed.** 24, 180, 205, 230, 238, 238, 255. | `cc_tap_timing/cc_note_*.png` |

## Issues (most severe first)

### P2

1. **[P2] Two reds on the face while a recording runs (§2.2).**
   - Screen: home face page, memo or track recording in the background.
   - The REC chip's red dot appears next to the face's own red:
     - DOT: the red centre dot at (233,233) plus the chip dot at (167,421) (`r3_rec/face_rec.png`);
     - GLYPH: the red seconds dot at (90,376) plus the chip dot at (167,334) (`r3_rec/glyph_rec.png`).
   - The CC header is fine, with one red (`r3_rec/cc_rec.png`).
   - **Fix:** recording is the one red that matters, so the face yields:
     - add `face_ctx.rec` (set in `build_ctx` from `H.rec_raw > 0`);
     - faces draw their red accents in C_PRI while it is set, as d00442b already does for the unread dot ("red where the face has no other red");
     - fade the swap with the chip's 4 steps.
2. **[P2] A double press opens the launcher, reverses it, then irises the app (r2 #26 regressed).**
   - Screen: face, button double press.
   - The launcher iris grows over f139-145 to about 80 % of the glass, with the list readable. It shrinks back over f146-150, home shows at f151, and the app iris starts at f152 (`r2_double/dbl.png`).
   - That is a visible reversal, 13 frames of motion before the app's iris even starts, and later than round 2 (f147).
   - **Fix:** when `IN_BTN_DOUBLE` arrives while the launcher iris is still opening (`sys_launcher.c:384`), don't close it.
     - Swap the iris's over-screen in place: `T.over = last`, call `last->enter()` with the 40 % build delay, set `s_stack[1].s = last`, and keep `T.tw` running.
     - The same circle then keeps growing, now showing the app: one continuous 0.34 s iris, no list after that frame, no reversal.
     - Expose this as `nos_retarget(const nos_screen *)` in nos.c.
3. **[P2] Pressed states still flash when a drag starts slowly (r2 #7 residue).**
   - Screens: notification list, launcher, widget pages.
   - With a finger that covers under 12 px in its first 90 ms (`drag` ease-in-out, 400 ms), the pressed visual shows and then drops when the slop passes:
     - card scroll: card lit at f241, 1 frame (`r3_pressflash/sc.png`);
     - card swipe: lit f273-274 (`sw.png`; also `notifs/nt_swipe_small.png` f294);
     - launcher: disc lit at f406 (`ls.png`);
     - Timer widget: the 5 MIN chip is lit f740-743, still lit 2 frames after the page has started moving (`pw.png`).
   - Settings no longer does this (6e4575e, 8 px).
   - **Fix:** use the same rule in `press_on` of `sys_notifs.c`, `sys_launcher.c` and `sys_power.c`, and in `pr_tick` in `sys_home.c`: treat the press as cancelled while `nos_touch_travel() > 8`.
     - For widgets, call `press(...,0)` the moment travel exceeds 8, and have `time_timer.c` drop its lit chip at once on `press(0)` (no hold).
4. **[P2] The banner brakes into the bezel on its timed exit, and creeps on arrival.**
   - Screen: any, banner.
   - **Exit:** a critically damped spring. The card's bottom edge goes 188, 171, 138, 104, 76, 53, 36, 25, 16, 10, 6, 3, 2 (f183-196). For 9 frames a grey sliver crawls at the top of the glass, each frame repainted (2.6 ms) (`r3_toast2/tq2.png`, `frames.csv`). It also delays a queued toast to f200.
   - **Arrival:** the bottom edge moves 33, 29, 23, 16, 13, 8, 6, 4, 2, 2, 1, 1, 1 px. That is 14 frames, the last 6 at 2 px or less (`banner/bn_in.png`).
   - §3: non-finger motion is `mtween`, arrivals EASE_STANDARD 300-420 ms, departures EASE_ACCEL, and never brake an exit.
   - **Fix (nos.c):**
     - arrival: an `mtween` over 0.30 s with EASE_STANDARD;
     - timed exit: an `mtween` over 0.20 s with EASE_ACCEL, finishing at `B.y.x = 0` and clearing `B.active` when it ends;
     - keep the spring only for the finger release (`msp_kick` on flick-away).

### P3

5. **[P3] Button closes hesitate mid-exit.**
   - `EASE_BTN_EXIT` (0.25, 0.35, 0.8, 0.6) runs fast-slow-fast:
     - the CC goes 81, 63, 57, 58, 62, 79 px per frame;
     - the power sheet goes 90, 64, 56, 58, 62, 78;
     - evidence: `r3_btnclose/ccb_*`, `pwb.png`.
   - No cubic gives a monotonic speed with a first step of 55 px or more.
   - **Fix:** in `nos_home`, leave at constant speed like the flick exit: `T.use_tween = 0; T.lin_v = 1.0f / 0.22f;`. That is about 71 px per frame from the first frame.
6. **[P3] CLEAR ALL leaves 4-5 frames of black glass before the empty state.**
   - The last item is off the glass at f275; nothing is drawn f276-279; the bell starts at f280 (`notifs3/nt_clear.png`).
   - **Fix:** set `N.t_empty = N.t_clear + (N.clear_n - 1) * 2 / NOS_FPS + 0.15f` when the clear starts, so the bell builds while the last card exits.
7. **[P3] The banner tap has no pressed state (r2 #25).** **Fix:** fill the card one step up (C_LINE2) for 3 frames before it flies up.
8. **[P3] The CC header group is 15 px left of centre.**
   - "10:09 · 78%" ink spans x 154-282 (centre 218) because the separator dot is pinned at 233 (`control/cc_open.png`).
   - **Fix:** centre the whole group: `x0 = UI_CX - (wt + 24 + wb) / 2`.
9. **[P3] The power commit crossfade has a blank frame.**
   - f211 shows the empty red pill between "TAP AGAIN TO POWER OFF" and "[ POWERING OFF ]" (`r3_power/com_o.png`).
   - **Fix:** overlap the two: `ka = q8(1 - c)`, `kd = q8(c)`.
10. **[P3] The face picker has no cancel any more.**
    - Tap and button both choose. The edge swipe now pages (fine), but `sys_faces.c`'s header still says "Left-edge swipe cancels".
    - **Fix:** make `IN_BTN` restore `nos_prefs.face` (cancel), keep tap as choose, and update the comment.
11. **[P3] Residual green AA tint.**
    - About 250 px per frame on the notification list are 2-3 G steps above neutral: card icons (92,121) (106,113,106) and "NOW" labels (354,127) (90,97,90).
    - The CC brightness icon's dim dots are (8,20,8) at (110,383) (`r2_reveal/nt_list.png`, `control/cc_open.png`).
    - The source is dot-stamp and label AA on dark fills, not `gfx_blend565`.
    - **Fix:** clear green's low bit in the dot.c mask blend and the `ui_text` AA blend, as b4f6492 did in gfx.
12. **[P3] (cross-team, con_music.c) The Music playhead dot pops in.** It appears in 1 frame at f202, when the ring build ends (`r2_pages/music_ring_strip.png`). **Fix:** reveal it with the last ring dot over 3 colour steps.

## Cost and damage

- 36 runs, 0 late frames.
- p50 is 1.5-17 ms. p95 is 24.4 ms only where a full-screen sheet, iris or dissolve moves (bus-bound).
- `--damagecheck` PASS on: home, control, notifs, notifs3, launcher, banner, faces_power, r2_pages,
  r2_reveal, r2_tail, r2_double, r3_power, r3_aod, r3_btnclose, r3_pressflash, r3_toast2 (`dc_*`).
- Waste found: the banner exit creep (#4, 9 frames) only.

## Already excellent (don't break)

- **Reveals cost nothing:**
  - the launcher after an app;
  - the CC after Settings;
  - the list after the detail;
  - the AOD wake from the AOD dots.
- **Button home:**
  - leaves from wherever the sheet is (mid-drag, mid-open);
  - follows the sheet's geometry;
  - has no tail;
  - an iris closes toward the button.
- **Widget pages:** they build on the first visible frame, never at settle.
- **Iris:** a hard edge with a hairline rim, the icon seed, the app shown from frame 2, no black frames, taps land during it.
- **Power sheet:**
  - on the §8 grid;
  - the armed pill grows from the pressed fill, its partner gives way, one red at a time;
  - the committed state is shown in the pill.
- **Notifications:**
  - r40 C_SURF2 cards, a legible morphing count, the OPEN/DISMISS pair;
  - the standard empty state;
  - CLEAR ALL with the button in the stagger;
  - fly-out with an early gap close;
  - a cancelled swipe stays dark.
- **Toasts:** they drop in at full colour, fit the chord at any length, and queue behind banners. The banner yields to sheets.
- **Long press:** the rim charges continuously into the picker. The picker's moat keeps the dots whole.
- **AOD entry:** a 3 + 3 frame ramp from the face, and up from black when collapsed from elsewhere.
- **CC:** a monotonic toggle ramp, a centred slider, outlined nav tiles, absent tiles that explain themselves.
- **REC chip:** a per-face slot, a 4-frame fade, and a tap that irises the recorder from the chip.
- **Boot:** wordmark out, one black frame, then the face build.
