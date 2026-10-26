# Shell critique, round 2

Area: boot, home pager and widget pages, control centre (CC), notifications, launcher, face
picker, power sheet, banners and toasts, the back gesture and side button everywhere, AOD. I
also checked these screens against the §8 standards in docs/NOS_DESIGN.md.
Sim: `build/nos_sim_crit_shell.exe`, rebuilt at 5e1bd36. All 26 runs below were produced by
that build. Evidence is under `build/nos/critic/shell/r2/`, one folder per script, and paths
below are relative to that folder. Scripts are in `build/nos/critic/shell/scripts/`: all
round-1 scripts plus 8 new ones (`r2_reveal`, `r2_glyph`, `r2_fp_choose`, `r2_double`,
`r2_pages`, `r2_cc2`, `r2_iristap`, `r2_tail`). `tools/hairline.py` traces the sheet edge per
frame. Frame numbers are the `fNNN` labels in the contact sheets. Nothing ran on the board
(COM3 untouched).

## Verdict

Round 1's engine work landed, and it shows:
- Notification momentum works.
- The invisible transition tail no longer eats taps.
- The CC arrives built, and sheets have a hairline edge.
- The side button no longer waits on home.
- Long press works from everywhere.
- Taps land during the iris.
- The banner, CLEAR ALL and AOD wake were rebuilt properly.

The finger-driven sheets now feel right: 1:1 tracking with a constant slop, and a constant-speed
flick exit. Of the 37 round-1 items, 28 are fixed, 7 are partly fixed (one with a regression),
and 2 are still broken.

The new problems are mostly about rebuilds, in the places users go back and forth most:
- **P0:** going back to the launcher from any app shows a full black frame, then rebuilds the list.
- **P1:** widget pages slide in fully drawn, then blank and rebuild when the settle ends.
- **P1:** toasts at the top are wider than the glass there, and they sit on top of banners.

§8 compliance in the shell is weak:
- Restart commits on one tap.
- The power sheet's buttons and hero are off-spec.
- Notification cards, detail buttons and the empty state ignore the standards.
- Pressed states last 2 frames instead of 3, and flash at the start of every scroll.

Performance is clean: 0 late frames in 9,845 frames, and `--damagecheck` passes on 10
scripts.

## Round-1 verification

| # | Round-1 issue | Status | Evidence (r2/) |
|---|---|---|---|
| 1 | P0 no momentum in notifications | **Fixed.** A flick coasts and decays over f238-f252, with no jump at release. | `notifs/nt_scroll.png` |
| 2 | P1 transition tail eats taps, full repaints | **Partly fixed.** A tap 300 ms after the CC lands toggles Wi-Fi. The open settle ends at `tail/open_012`. The button-driven close still has a long ease-out tail (new #16). | `cc_tap_timing/tap300.png`, `tail/open.png`, `tail/close.png` |
| 3 | P1 CC flick-open arrives empty | **Fixed.** Content is drawn at full colour as soon as the sheet uncovers it. | `control/cc_flick.png` |
| 4 | P1 sheets have no edge | **Fixed.** Hairline plus grabber on both sheets. | `control/cc_pull.png`, `notifs/nt_pull.png` |
| 5 | P1 button waits 320 ms on home | **Fixed.** The iris starts at f30, the first frame after the press release. | `launcher/ln_open.png` |
| 6 | P1 long press only from home | **Fixed.** Opens from an app and from the CC. | `faces_power/pw_from_app.png`, `pw_from_cc.png` |
| 7 | P1 banner tap leaks `s_from_detail` | **Fixed.** The list after a banner tap is fresh and the unread state is correct. | `banner2/b2_sel.png` |
| 8 | P1 notifications close only at scroll 0 | **Fixed.** Pull-down closes from a scrolled list; `expect` passes. | `notifs3/nt_close_scrolled.png` |
| 9 | P1 button mid-drag snaps the sheet open | **Still broken.** See new #3. | `home_mid/dp.png`, `button/bt_while_drag.png` |
| 10 | P1 iris blocks input ~9 frames | **Fixed.** The glass is covered by f33. A tap 3 frames into the iris lands, and `expect top ACTIVITY` passes. | `iristap/it.png` |
| 11 | P1 no unread indicator on the face | **Fixed.** The face shows a white dot. The widget red dot fades in at settle f17-f21. | `pager_slow_full/ps_017..021.png` |
| 12 | P1 picker name pill on the face | **Fixed.** The face is dimmed and the pill has a keyline. P3 residue: new #28. | `faces_power/fp_rest_pill.png` |
| 13 | P1 confirming a face makes it jump | **Fixed.** GLYPH brightens over 5 frames with no rebuild. | `glyph/fp_glyph.png`, `fpc/fp_choose.png` |
| 14 | P1 AOD entry animates a sheet | **Fixed in firmware code** (`nos_main.c:249` calls `nos_home_now`). The sim's `set aod` still animates (new #27). | `aod_cc/aod_cc_home.png` |
| 15 | P1 waking from AOD blanks the time | **Fixed.** Brightens from the AOD dots in 4 frames. | `aod/aod_out.png` |
| 16 | P1 CLEAR ALL is a hard cut | **Fixed.** Cards fly out staggered, then the bell, then the text. Residue: the pill vanishes in 1 frame at f265 (new #30). | `notifs3/nt_clear.png` |
| 17 | P1 toasts on thumb-zone actions | **Partly fixed.** Toasts moved to the top, and power shows its state in the pill. They are not width-capped, so they are clipped by the bezel and cover banners (new #4). | `banner/to_banner_top.png`, `faces_power/pw_restart2.png` |
| 18 | P1 banner overflow, second red | **Fixed.** Ellipsis plus a white bell. | `banner/bn_long.png` |
| 19 | P2 back swipe moves vertical sheets sideways | **Partly fixed.** The CC back swipe is inert. The face picker still pushes sideways (new #17). | `control/cc_backswipe.png`, `faces_power/fp_cancel.png` |
| 20 | P2 fling-to-dismiss brakes | **Fixed.** The hairline goes 419, 340, 231, 153, 75: a constant ~78 px/frame exit. | `control/cc_flickclose.png` |
| 21 | P2 jump by the touch slop | **Fixed.** Recomputed against the sim's ease_inout drag: a constant 22 px offset, with no jump at lock. | `control/cc_pull.png`, `cc_slider.png` |
| 22 | P2 dimming tints greys green | **Fixed** for surfaces. Text AA fringe is still slightly green (new #29). | `reveal/nt_list.png` |
| 23 | P2 swipe-to-dismiss | **Fixed.** The gap closes early (f343), and the count morphs over f346-348. | `notifs/nt_swipe_flick.png` |
| 24 | P2 banner cramped and greedy | **Fixed.** Top 52, 300 wide (x 83-382), 2 lines. A top-edge pull opens the CC through it. | `banner/bn_in.png`, `bn_pull.png` |
| 25 | P2 toggle-off blob | **Fixed.** | `control/cc_toggle.png` |
| 26 | P2 nav tiles look like toggles | **Partly fixed.** The Settings tile is fine. Torch still renders as an off toggle (new #20). | `cc2/sel.png`, `cc2/torch_tap.png` |
| 27 | P2 iris deletes the tapped icon, closes stale | **Partly fixed.** Closing toward the button is fixed. Opening still deletes the icon and shows near-black frames (new #18). | `launcher/ln_app_btn.png`, `ln_tap_far_zoom.png` |
| 28 | P2 card padding, fixed height | **Fixed.** Variable height, balanced padding. | `notifs/nt_list_settled.png` |
| 29 | P2 launcher names cut by the bezel | **Fixed.** Outer names fit inside the glass. | `launcher/ln_fling.png` |
| 30 | P2 long press gives no feedback | **Fixed, then regressed.** The rim now charges, but it drops out for 4 frames at hand-off (new #15). | `faces_power/fp_open_sel.png` |
| 31 | P2 power arm: no timer, large red area | **Partly fixed.** Outline, 3 dots and a timer are present. The label and layout are off §8, and Restart has no arm (new #5, #6). | `faces_power/pw_armed.png`, `pw_sel.png` |
| 32 | P2 boot double exposure | **Fixed.** The wordmark goes out f41-42, then 1 black frame at f43, then the face builds. | `boot_full/bf.png` |
| 33 | P3 detail DISMISS no closure | **Fixed.** The list slides up ~30 px over f267-f275. | `notifs2/d_dismiss.png` |
| 34 | P3 AOD entry one-frame cut | **Still broken** (new #19). | `aod/aod_in.png` |
| 35 | P3 toast copy inconsistent | **Fixed.** Brackets are consistent. | `banner/to_toast.png` |
| 36 | P3 CC header note pops in | **Fixed.** "WI-FI ON" fades in over f235-237. | `cc_tap_timing/cc_note.png` |
| 37 | P3 Haptics icon is a palm | **Fixed.** Vibrating-phone icon. | `control/cc_open.png` |

## Issues (most severe first)

### P0

1. **[P0] Back to the launcher: a full black frame, then the list rebuilds.**
   - Screen: launcher, revealed by the back swipe from any app.
   - At f174 the launcher's first revealed frame is a full-screen black repaint. `frames.csv`
     row 174 shows `calls 0`, `full 1`, screen LAUNCHER.
   - The rows then fade up one by one over f175-f183 (`tail/back_013.png`,
     `tail/back_strip.png`). The user had just watched that list slide in fully drawn under the
     app.
   - This breaks §8 Entry build ("screens revealed by back don't rebuild"). It is the most
     common navigation in the OS.
   - Cause: `sys_launcher.c` `l_enter` sets `S.t_enter = nos_t` unconditionally, so every row's
     level is 0 on reveal.
   - **Fix:** `if (!nos_revealed()) S.t_enter = nos_t; else S.t_enter = nos_t - 1.0f;` (any
     value older than the full stagger). The same pattern is already right in the CC and
     notifications (`reveal/cc_set_back.png`, `reveal/nt_det_back.png`).

### P1

2. **[P1] Widget pages rebuild after they have slid in fully drawn.**
   - Screen: home pager, face to widget pages, slow drag and flick.
   - **Timer page:** slides in complete, blanks to black at f159, then rebuilds over f160-f169
     (`pages/pg5.png`).
   - **Weather page:** the hero and icon blank at f93 and rebuild, and the city text hard-swaps
     LONDON to BENGALURU at f96 (`pages/pg3.png`).
   - **Music page:** the progress ring re-sweeps over f211-f214 on the way back
     (`pages/pg_back.png`).
   - Cause: `sys_home.c:334-337` calls the widget's `shown()` when the settle ends
     (`p != H.shown_page`), and the widgets' `shown()` reset their build:
     - `time_timer.c:841`: `W.t0 = nos_t; W.build = 0;`
     - `out_weather.c:549`: `G.t0 = nos_t; G.frame = 0;`
     - `con_music.c:470`: `G.t_shown = nos_t`
   - **Fix:** call `shown()` on the rising edge of the slot's visibility (the first frame any
     part of the page is on screen), not at settle. Alternatively, pass a flag so `shown()`
     skips the build reset when the page was already visible during the drag.
   - Fetch the weather city before the page is first drawn, or crossfade it with
     `dot_text_morph` (§8 State changes).

3. **[P1] A button press while a finger drags a sheet still snaps it open, then closes it
   (r1 #9).**
   - The CC is at y 79 over f101-f103. At f104 it snaps fully open (hairline 416), then runs a
     full close (`home_mid/dp.png`). The same happens in `button/bt_while_drag.png`, f189 to
     f190.
   - Cause: `nos_home()` sets `T.state = TXS_IDLE` before `nos_pop()`, so `nos_pop` reads
     `from = 1.0f`.
   - **Fix:** in `nos_home`, take `float p = (T.state != TXS_IDLE) ? T.p : 1.0f;` before
     touching `T.state`, and start the pop from `p`
     (`tx_begin(tx, under, over, p, 0)`, or pass `p` into `nos_pop`). The sheet then closes from
     y 79 in about 3 frames.

4. **[P1] Top toasts are clipped by the bezel and land on banners.**
   - "[ OPENED ON PHONE ]" is x 88-377, y 30-71, centre y 50. The glass chord at y 30 is only
     229 px (x 119-347), so both rounded ends are sliced off. It also covers the header of the
     banner showing at the same time (`banner/to_banner_top.png`).
   - "[ RESTART (SIM) ]" behaves the same (`faces_power/pw_restart2.png`).
   - **Fix:** in `ui_draw_toast`:
     - cap the pill width at `2*ui_half_width(top) - 24`;
     - for long copy, move the pill down: top y 58 gives 283 px;
     - ellipsize beyond that.
   - While a banner is up, queue the toast until the banner leaves, or draw it at the banner's
     bottom + 8 px.

5. **[P1] (§8) Restart commits on one tap, and its label is a hard cut.**
   - §8 requires arm-then-tap-again for restart as well as power off.
   - One tap on RESTART swaps the label to "[ RESTARTING ]" in a single frame, f595 to f596
     (`reveal/pw_restart_f.png`).
   - Power off arms, but:
     - the label is "TAP TO POWER OFF", not "TAP AGAIN TO POWER OFF";
     - the 3 dots sit below the pill at y 440 instead of inside it (`faces_power/pw_armed.png`);
     - it differs from Settings' reset, which is the correct model.
   - **Fix:** move `arm_pill` from `set_settings.c:127` into `ui.c` as `ui_arm_pill`, and use it
     for both power actions. Its geometry: `UI_BTN_H` tall, 320 wide armed, label at cy-10,
     dots at cy+18 inside the pill.
     - Labels: "TAP AGAIN TO RESTART" and "TAP AGAIN TO POWER OFF".
     - On commit, crossfade the label to "[ RESTARTING ]" / "[ POWERING OFF ]" over 4 frames.

### P2

6. **[P2] (§8) The power sheet geometry is off-standard.**
   - The pills are 72 tall (`BH 72`; widths 288/288/256 at y 222/306/390).
   - The hit rows are 84 px, under the 88 minimum.
   - The hero "78%" is pitch 6 (52 px tall); §8 says hero ≤ 4 glyphs at pitch 9.
   - Evidence: `faces_power/pw_open.png`, `pw_rest.png`.
   - **Fix:**
     - hero at pitch 9, top y 118 (bottom ~181);
     - SLEEP / RESTART as a pair, 140x88 at x 154/312, cy 272;
     - POWER OFF as a single, 200x88 at cy 372.
   - An armed pill grows to 320 (per #5), and its row partner fades out over 150 ms.

7. **[P2] The pressed state flashes for 1-3 frames at the start of every scroll or swipe.**
   - Notifications: `nt_scroll` f235-236, `nt_swipe` f311-313, `nt_swipe_flick` f338-339.
   - Launcher: `ln_fling` f83-84.
   - The Settings row under an edge back swipe (`reveal/cc_set_back`, f90).
   - The Timer chip (`pages/pg_back`, f191).
   - **Fix:** this is a shared rule in the row/tile press helper, so fix it once there:
     - don't show the pressed visual until 100 ms (3 frames) after down;
     - cancel it silently if the finger passes the slop first;
     - if the up arrives first, show it on release for 3 frames.

8. **[P2] (§8) Pressed lasts only 2 frames on a quick tap.** §8 asks for ≥ 3 frames.
   - The sim's `tap` holds for 2 frames, and the pressed visual shows for exactly those 2 on:
     - CC tiles (`cc_tap_timing/cc_note`, f232-233);
     - power pills (f594-595);
     - launcher rows;
     - DISMISS and CLEAR ALL.
   - **Fix:** keep `press_until = nos_t + 0.10f` on up, and draw pressed while
     `nos_t < press_until`, as the weather widget already does.

9. **[P2] A cancelled swipe shows the armed (white) dismiss disc while the card springs back.**
   - The card peaks at ~89 px, never past the -110 threshold, yet the disc turns white at f008-009
     (`notifs/nt_swipe_008.png`, `nt_swipe_009.png`, `nt_swipe_zoom.png`).
   - Cause: in `sys_notifs.c`, `armed = N.sw_armed || N.out_i == i` is also true for the
     spring-back animation.
   - **Fix:** `int armed = N.sw_armed || (N.out_i == i && N.out.target < 0);`

10. **[P2] The banner stays on top of the CC.**
    - Pulling the CC down while a banner is up leaves the banner over the CC header and the
      first tile row for the rest of its life (`banner/bn_pull.png`, `bn_pull_after.png`,
      `bn_sel.png`).
    - **Fix:** when a system sheet (CC, notifications, power) begins, send the banner out
      upward with its flick-away motion over 150 ms. Don't re-show it over the sheet.

11. **[P2] (§8) Notification cards are r30 C_SURF.** They measure (16,16,16), x 61-404
    (`notifs/nt_list_settled.png`). §8 says r 40, C_SURF2 (#1A1A1A).
    - **Fix:** in `sys_notifs.c`, set `CARD_R 40` and fill with `C_SURF2`. The pressed fill
      goes one step above that (`C_LINE2` stays).

12. **[P2] The notification count numeral is illegible.**
    - It is about 15 px tall at pitch ~2, and "4" reads as "ч" (`notifs/count_zoom2.png`,
      `reveal/nt_count_zoom.png`).
    - **Fix:** use §8 secondary numerals (5x7, pitch 3, 21 px tall), or plain `ui_label` text,
      for example "4 NEW".

13. **[P2] (§8) The notifications empty state is custom, not `ui_absent`.**
    - `draw_empty` draws:
      - a ring r74 at y 178;
      - a white bell;
      - "No notifications" in UI_F_BODY at y 296;
      - `ui_state` at y 336, not 340.
    - The phone-absent variant uses its own copy (`notifs3/nt_empty.png`, `nt_empty2.png`).
    - **Fix:**
      - No notifications: `ui_absent(DOT_ICON_BELL, "No notifications", NULL, NULL, p)`, then
        `[ ALL CAUGHT UP ]` at y 340.
      - Phone absent: `ui_absent(DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as
        Bangle.js NOS", NULL, p)`.
      - Keep the ring-fill as `ui_absent`'s icon build if it can be shared.

14. **[P2] (§8) Notification detail buttons are 68 tall.**
    - OPEN ON PHONE is a pill at x 103-362, y 266-333. DISMISS is an outline at y 350-417
      (`notifs/nt_detail.png`, `nt_detail2_scrolled.png`).
    - **Fix:** `UI_BTN_H` 88. Use a pair, 140 wide at x 154/312, with "OPEN" as primary and
      "DISMISS" as C_SURF2 secondary. Place it at cy 372 when the body fits, or at the content
      end when it scrolls.

15. **[P2] Long-press feedback drops out for 4 frames at the hand-off (r1 #30 regression).**
    - The rim charges 16, 41, 57 over f33-f39, then reads 0 for f40-f43 when FACES takes over,
      then jumps to 98 (`faces_power/fp_open_014.png` to `fp_open_018.png`, `fp_open_sel.png`).
    - **Fix:** start the picker's own rim (or scale-out) at the press level it inherits. Pass the
      charge as the transition's start (`p0 = 0.57`) and draw the rim from frame 1.

16. **[P2] Button-driven closes keep a 13-frame ease-out tail (r1 #2 residue).**
    - After a press, the CC decelerates at -88, -72, -54, -37 and -26 px/frame.
    - The last ~47 px and the face un-dim creep over f83-f89.
    - Every one of those 13 frames is a 24.4 ms full repaint (`tail/close.png`).
    - **Fix:** non-finger pops use a fixed tween, `EASE_ACCEL` over 0.22 s (7 frames), so they
      leave at speed like the flick exit (#20).

17. **[P2] The face picker leaves sideways on the back swipe (r1 #19 residue).**
    - The left-edge swipe pushes the picker right (TX_PUSH) and reveals the old face from the
      left, like paging (`faces_power/fp_cancel.png`, f209-f217).
    - **Fix:** treat it as a modal. Dissolve back (TX_DISSOLVE, 0.20 s), or keep the swipe
      inert, as in the CC.

18. **[P2] Opening an app through the iris still deletes the tapped icon (r1 #27 residue).**
    - The icon disappears at f302, then f304-f305 are near-black before the app builds
      (`launcher/ln_tap_far_002.png` to `ln_tap_far_005.png`, `ln_tap_far_zoom.png`).
    - This breaks §8 "iris entries start their build at 40 % of the iris".
    - **Fix:**
      - draw the launcher icon sprite over the iris seed for the first 40 % (LUT level
        `1 - p/0.4`);
      - start the app's hero build at p = 0.4, so there is never a black frame.

### P3

19. **[P3] AOD entry is still a 1-frame cut** from the lit face to grey (`aod/aod_in.png`,
    r1 #34). **Fix:** 4-frame colour-step down, as on wake (#15 is the model).
20. **[P3] The Torch tile still looks like an off toggle but opens a screen**
    (`cc2/torch_tap.png`). **Fix:** give it the Settings tile's nav style (ring outline, no
    disc state).
21. **[P3] The CC slider group is 10 px left of centre.** Ink spans x 94-352, so its centre is
    ≈223. **Fix:** in `sys_control.c`, `SL_X 138`, `SL_ICON_X 114`.
22. **[P3] (§8) Home page dots use a local `DOTS_Y 448` and a custom drawer** (rows 444-451)
    instead of `ui_page_dots` at `UI_DOTS_Y` 440. **Fix:** use `ui_page_dots`, keep the linear
    fade, or exempt home in §8 with the reason.
23. **[P3] (§8) The launcher "APPS" header** sits at y ≈139-147 and scrolls with the list,
    unlike `ui_title` at y 70. **Fix:** use `ui_title`, or list the launcher as exempt in §8.
24. **[P3] The pressed launcher disc lifts to C_SEC with white ink**, which is low contrast
    (`launcher/ln_fling` f83, `ln_tap_far` f299). **Fix:** pressed disc `C_LINE2`, ink stays
    white.
25. **[P3] Banner tap has no pressed state** (`banner/bn_tap.png`). **Fix:** card fill one step
    up for ≥ 3 frames.
26. **[P3] A double press shows a fully drawn launcher for ~6 frames** before the
    last-app iris (`double/dbl.png`, f141-f147). **Fix:** on IN_BTN_DOUBLE, iris from the home
    screen straight to the last app.
27. **[P3] Sim fidelity: `set aod` and `home` animate**, while firmware calls `nos_home_now`
    (`aod_cc/aod_cc_home.png`). **Fix (sim):** make `set aod` mirror `nos_main.c:249`.
28. **[P3] The picker pill keyline cuts dots into crescents** at (178,381) and (283,381)
    (`faces_power/fp_rest_pill.png`). **Fix:** clear a 4 px moat around the pill before
    drawing the keyline.
29. **[P3] The text AA fringe is still slightly green under dimming.** About 150 px per frame
    sit 2-3 G steps high, for example (49,60,49) and (90,97,90) (`reveal/nt_list.png`,
    `reveal/nt_det_back_008.png`). **Fix:** use the same neutral mix for the text AA LUT.
30. **[P3] The CLEAR ALL pill vanishes in 1 frame** at f265 (`notifs3/nt_clear.png`).
    **Fix:** fade it out with the last card, over 4 colour steps.
31. **[P3] The toggle-on ramp dips:** 156, 82, 139, 197, 222, 255
    (`cc_tap_timing/cc_note_000.png` to `cc_note_007.png`). **Fix:** start the ramp from the
    pressed level (C_SEC).

## §8 compliance summary

| Rule | Shell status |
|---|---|
| Title y 70 | Sheets are exempt. The launcher "APPS" header is non-standard (#23). |
| Buttons 88 / r44 / y 372 | Fails on the power sheet (72, #6) and notification detail (68, #14). |
| Arm-then-tap-again | Restart has none (#5). The power-off label and dot placement are off-spec (#5). |
| Pressed ≥ 3 frames | 2 frames on a quick tap (#8). It also flashes at scroll start (#7). |
| Cards r40 C_SURF2 | Notifications use r30 C_SURF (#11). |
| One red | Passes everywhere checked. Banners use a white bell, and the face unread dot is white. |
| Dot numerals | The power hero is pitch 6 (#6). The notification count is pitch ~2 (#12). |
| Empty / phone absent | Notifications are custom (#13). |
| Entry build, no rebuild on reveal | Fails on the launcher (#1) and widget pages (#2). The iris 40 % rule fails (#18). The CC, notifications and picker pass. |
| State changes, no hard cut | Restart label (#5), weather city (#2), AOD entry (#19) and the CLEAR ALL pill (#30) fail. |
| Page dots y 440 | Home uses 448 with a custom drawer (#22). |
| Toasts y ≈46 | Position is right, but there is no width cap (#4). |

## Cost and damage

- 26 runs, 9,845 frames, 0 late.
- p50 is 1.5-11 ms. p95 is 24.4 ms only in runs with full-screen sheet or iris motion (bus
  bound).
- `--damagecheck` passes (every frame identical to a forced full repaint) on: home, control,
  notifs, notifs3, launcher, banner, faces_power, r2_pages, r2_reveal, r2_tail (`dc_*`).
- The only waste found is the 13-frame close tail (#16), at 24.4 ms each.

## Already excellent (don't break)

- **Pager:** slop absorbed, velocity picks the page, the alias trip makes page 5 → face one
  page of motion, no overshoot.
- **CC:**
  - 1:1 tracking with a constant 22 px slop;
  - a constant ~78 px/frame flick exit;
  - built on arrival;
  - grabbing mid-flight works (`nos_sheet_take`).
- **Notifications:**
  - momentum with edge fade;
  - fly-out with an early gap close and a count morph;
  - staggered CLEAR ALL;
  - DISMISS closure slide;
  - an empty state pulled up without a rebuild (`notifs3/nt_empty_open.png`).
- **Reveal without rebuild:** the CC after Settings and the list after detail
  (`reveal/cc_set_back.png`, `reveal/nt_det_back.png`). Copy this into the launcher (#1).
- **Banner:** 300 wide, top 52, 2 lines, white bell, ellipsis, spring in, flick away.
- **Launcher:**
  - the iris closes toward the button;
  - taps land during the iris;
  - outer names fit;
  - flings land on a row.
- **Side button:** no 320 ms wait. A double press opens the last app. A long press opens the
  power sheet from everywhere.
- **Face picker:** confirming GLYPH brightens with no rebuild.
- **Boot:** clean wordmark out, one black frame, then the face build.
- **AOD wake:** brightens from the AOD dots in 4 frames.
- **Module-absent CC tiles:** they explain themselves ("NO LTE MODULE", `cc2/absent_tap.png`).
- **Damage tracking:** exact on every script checked.
