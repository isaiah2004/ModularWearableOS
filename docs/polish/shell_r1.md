# Shell critique, round 1

Area: boot, home pager, control centre (CC), notifications, launcher, face picker, power sheet,
banners/toasts, back gesture + side button, AOD. Simulator: `build/nos_sim_crit_shell.exe`
(rebuilt mid-review to pick up the in-flight `sys_home.c` page-dot fix). Scripts:
`build/nos/critic/shell/scripts/*.txt`. All evidence paths are under `build/nos/critic/shell/`.
Frame numbers are the `fNNN` labels in the contact sheets.

## Verdict

The shell has the right bones. The home pager and launcher really are Apple-grade: 1:1
tracking, the release velocity picks the page or row, there is no overshoot, and a fling lands
exactly on a row. Every script passes the damage check, and not one frame is late anywhere (worst
case 24.4 ms, bus-bound). The finger-driven sheets are a different story, and they are what
people touch most. Momentum scrolling in the notification list is broken outright. Every engine
transition keeps a long, near-invisible spring tail that blocks touches: tiles tapped 300 ms
after the CC lands are ignored. The CC arrives empty on a flick because its build-in runs top to
bottom while the sheet reveals bottom to top. The sheets have no visible edge, so the face looks
sliced rather than covered. Vertical sheets leave sideways on the back swipe. The side button
waits 320 ms on every home press. Long press does nothing outside home. On top of that there are
state bugs: the banner-tap path leaks `s_from_detail`, and a button press mid-drag snaps the CC
back open. A user would hit several of these within a minute. Fix items 1-8 before any visual
polish.

## Issues (most severe first)

1. **[P0] Notifications list + detail: no momentum, ever.** A flick (380→200 px in 150 ms) moves
   the list exactly as far as the finger and stops dead on the release frame. `nt_scroll.png`
   f238-f240 moves, then f241 onward is `idle`, and `nt_scrolled.png` shows the same position.
   Cause: `process_touch` sends `IN_DRAG_END` then `IN_UP`. `n_input` IN_UP does
   `if (!N.dragging) msc_release(&N.sc, 0)`, but `N.dragging` was cleared one call earlier, so the
   fling velocity is overwritten with 0. `d_input` (detail) has the same bug. **Fix:** in both
   IN_UP handlers, release only if the scroller is still grabbed:
   `if (N.sc.dragging) msc_release(&N.sc, 0);` (and `D.sc.dragging` for the detail).

2. **[P1] Engine transitions keep a 250-300 ms invisible tail that eats taps, repaints the whole
   screen and crawls at the bezel.**
   - CC opened by drag: released at f46, the picture stops at about f55, but `TXS_AUTO` runs to f63
     with full 217k-px repaints (`control/frames.csv`). A tile tapped 300 ms after release is
     swallowed: `press_begin` grabs the moving transition, so Wi-Fi stays on
     (`cc_timing/tap300.png`). The same tap at 1.5 s works (`cc_timing/cc_note.png`). The first
     tap in `control/cc_toggle.png` was lost the same way.
   - Button close of the CC: the sheet is gone by f361, but full repaints continue to f370.
   - Visible crawl: after a back swipe from an app, a 2 px sliver of the app sits at the right
     bezel for 9 frames (`launcher/ln_app_back.png` f338-f346). The same happens with the CC
     torch tile (`control/cc_backswipe.png` f486-f490), the picker rim
     (`faces_power/fp_cancel.png` f217-f222) and the detail (`notifs/nt_detail_back.png`
     f392-f396).

   **Fix (nos.c):**
   - `tick_transition`: end when `|x-target| < 1.5 px && |v| < 90 px/s` (now 0.5 / 30).
   - `press_begin`: grab only if `fabsf(T.sp.v) > 150 && fabsf(T.sp.x - T.sp.target) > 6`.
     Otherwise snap to the target, run the normal completion (including the pop when
     `p <= 0.001 && popping`) and fall through to `send(IN_DOWN)` so the tap reaches the screen.

3. **[P1] CC flick-open: the sheet arrives empty and its content builds in afterwards.** The
   `tile_in()` stagger runs top row first, in time since `c_enter`. A pull-down reveals the
   bottom row first. At `control/cc_flick_strip.png` frames 4-6, the sheet covers about 380 px
   but shows only 2-5 tiles in grey: no slider, no bottom row, and a black void where they
   belong. The slider appears 8 frames after the gesture starts. On a slow pull
   (`control/cc_pull.png` f35-f38), the bottom tiles fade in from grey while sliding.
   **Fix:** when the screen is entered through `nos_pull` (finger-driven), skip the build-in
   (`C.t_enter = nos_t - 1`). Keep it only for non-finger entries. Apply the same rule to the
   notification cards' rise.

4. **[P1] Sheets have no edge, so the face looks sliced.** CC and notifications are black on
   black, so the moving edge is invisible. The face under them is cut by a horizontal line
   through its dot digits, with nothing marking the cut: `control/cc_pull.png` f34 (the top of
   "10" chopped flat), `notifs/nt_pull.png` f197 ("09" halved), `notifs3/nt_empty_open.png`
   f338. It reads as a rendering glitch, not a sheet. **Fix (scene_draw, TX_RISE/TX_DROP):**
   - After drawing `over`, fill a 2 px `C_LINE2` (#333) line across the glass at the sheet's
     leading edge (`y = top` for rise, `y = bot-2` for drop).
   - Draw a 36x5 px `C_DIS` grabber pill 12 px inside that edge, centred.
   - Strengthen the under-dim from `32-20p` to `32-26p` so the covered face recedes.

5. **[P1] The side button on home waits 320 ms for a possible double press, every time.**
   - Face → launcher: button down at f26, iris starts at f39, 13 frames (433 ms) after the press
     (`launcher/ln_open.png`).
   - Widget page → face: press at f171, motion starts at f184 (`home/btn_to_face.png`).
   - On pushed screens, a press during any transition also takes the double-press path
     (`process_button`: `else if (s_depth > 1 && !T.state)`). The second press in
     `button/bt_mid_iris.png` acts about 15 frames later.

   **Fix:**
   - On pushed screens, drop `&& !T.state`.
   - On home: if `sys_launcher_last()` is 0, or the pager is not on the face, dispatch `IN_BTN`
     on release immediately.
   - Otherwise start the launcher iris on the first release. If a second press arrives within
     320 ms, `nos_replace(last_app, TX_IRIS)`.

6. **[P1] Long press opens the power sheet only from home.** Spec §4 says "Button long press ->
   power sheet". In the stopwatch (`faces_power/pw_from_app.png`) and the CC
   (`faces_power/pw_from_cc.png`) nothing happens. `dispatch_btn` has no fallback for
   `IN_BTN_LONG`. **Fix:** in `dispatch_btn`, if the screen doesn't consume `IN_BTN_LONG` and the
   top isn't `scr_power`, call `nos_push(&scr_power, TX_RISE)`.

7. **[P1] Banner tap leaks `s_from_detail`: the next notification list is stale and unread
   never clears.** `banner_open` → `sys_notif_open(0)` sets `s_from_detail = 1` without the list
   ever being under the detail. After backing out and pulling up the list:
   - the cards appear without their rise (`banner2/b2_pull.png` f137-f147 are static, compare
     `notifs/nt_pull.png`);
   - the scroll position is stale;
   - `svc_notif_mark_all_read()` is skipped, so the red unread dot is still on the widget pages
     after the list was viewed (`banner2/b2_page1_unread.png`).

   **Fix:** delete `s_from_detail` and branch on `nos_revealed()` in `n_enter`. The engine
   already flags a reveal.

8. **[P1] Notifications can't be closed with a pull-down unless the list is already at exactly
   0.** With about 40 px of residual scroll, a 270 px downward drag scrolls to the top, then
   rubber-bands the cards to y≈330 and springs back. The sheet never moves
   (`notifs3/nt_close_top.png`, and `expect top HOME` failed at f141). **Fix:** hand over
   mid-gesture. When `N.dragging` and the next `msc_drag` would go below 0, call a new engine
   entry, `nos_sheet_take(in)`. It runs `finger_begin(tx, under, top, 1, 1, AXIS_Y, sign)` with
   `P.y0 = P.y`, `P.owner = 1`, and returns 0 for the rest of the drag.

9. **[P1] A button press while a finger drags a sheet makes the sheet close, then snap fully
   open in one frame.** In `button/bt_while_drag.png`, the CC is being pulled; the button at
   f188 closes it (f193-f198). The next touch sample makes `finger_update` overwrite `T.p`: f199
   shows the face, f200 the fully open CC, then it closes again. **Fix:** in `dispatch_btn`, if
   `P.owner == 1`, set `P.owner = 3` (finger inert until release) before acting.

10. **[P1] The launcher iris blocks input for about 9 frames after the glass is already
    covered.**
    - `far` is measured to the bounding-square corners: 521 px from (466,233), while the glass
      needs only 466.
    - `EASE_STANDARD` covers the glass by f43, frame 5 of 13 (`launcher/ln_open.png`).
    - Tween transitions set `P.owner = 3`, so touches on the visible launcher are dropped until
      f52.

    **Fix:**
    - Use `far = hypotf(fx - GFX_CX, fy - GFX_CY) + UI_R` and set the duration to 0.34 s.
    - In `press_begin`, a press during a tween finishes it instantly (`T.p = T.tw.to`, complete)
      and delivers `IN_DOWN`.

11. **[P1] No unread indicator on the face.** `sys_home.c` says "the face draws its own
    (face_ctx.unread)", but no face references `unread`
    (`grep unread nos/faces/*.c` → only face.h). With 5 unread, the face shows nothing
    (`home_v2/face_unread.png`). The dot also pops in at full size when the pager crosses 0.5
    (`home/pager_slow.png` f36→f37). **Fix:**
    - Draw the indicator for page 0 as well: `dot_disc(UI_CX, 16, 8, C_WHITE)`, white because
      face_dot already uses its one red for the seconds.
    - Keep red on widget pages.
    - Fade it with the slot offset instead of the 0.5 threshold.

12. **[P1] Face picker: the name pill sits on the face's content.** The white "DOT" pill
    (y 384-424) covers the "78%" battery label and the lowest dot row of "09"; the label peeks
    out below it (`faces_power/fp_rest.png`). **Fix:**
    - Dim the live face while in edit mode (expose `dim_rows` as `nos_dim(fb, k)`, k = 18/32).
    - Give the pill a 6 px `C_BG` keyline (draw an `ui_rrect` 12 px larger in `C_BG` first).

13. **[P1] Confirming a face makes it jump.** In `faces_power/fp_choose.png`, f115 shows GLYPH at
    10:09 with its tick ring. At f116 the ring and date vanish and the hands teleport to about
    8:20, then sweep back to 10:09 over 10 frames (f116-f126). The face was already live and
    full-size, so the "build-in as confirmation" reads as a glitch. **Fix:** remove
    `sys_home_build_in()` from `sys_faces.c finish()`. The rim fade plus `FB_SUCCESS` is the
    confirmation.

14. **[P1] AOD entry with any screen open animates a full-bright sheet, then repaints 11
    identical frames.** The firmware calls `nos_home()` on `PWR_AOD` (`nos_main.c:241`). In
    `aod_cc/aod_cc_home.png`, the white CC tiles slide away over the AOD face for 6 frames, then
    f64-f74 are full 24.4 ms repaints of an unchanged AOD face. That costs power and risks
    burn-in. **Fix:** add `nos_home_now()` (collapse to root, `TX_NONE`, `s_force_full = 1`) and
    call it on `PWR_AOD`. The sim's `set aod` should do the same, so scripts match the device.

15. **[P1] Waking from AOD blanks the time.** The wake build-in starts from nothing:
    - at f1856 the grid is unlit and no time is shown;
    - the hour is legible from about f1861;
    - the minutes from about f1864, which is 270 ms (`aod/aod_out.png`).

    The AOD showed the time a frame earlier. **Fix:** in `home_tick`, when
    `H.last_aod && !nos_aod`, don't call `sys_home_build_in()` (set `H.c.enter = 1`). A brighten
    from the AOD dot positions is the most you want there.

16. **[P1] CLEAR ALL is a hard cut.** All cards vanish in one frame (f264→f265) and
    "No notifications / [ ALL CAUGHT UP ]" appear instantly. Only the bell and ring build in
    afterwards, so the text arrives before its icon (`notifs3/nt_clear.png`). **Fix:**
    - Fly the visible cards out left with the existing `out` spring (target
      `-CARD_W-CARD_X-20`, v = -900), staggered 2 frames per card, top first.
    - Call `svc_notif_clear()` when the last one leaves.
    - Start `t_empty` then. Text builds 4 frames after the bell (`ui_text` alpha by
      `p > 0.3`).

17. **[P1] Toasts land on the thumb-zone actions.** The toast pill is at y 352-398.
    - Restart: "[ RESTARTING ]" sits on top of the POWER OFF pill, with the red outline peeking
      out underneath (`faces_power/pw_restart.png`).
    - In the stopwatch, "SAVED" / "OPENED ON PHONE" cover LAP/START (`banner/to_toast.png`,
      `banner/to_banner.png`).

    **Fix:**
    - Move toasts to the top (`y = 46`, max width 270 px fits the chord there).
    - For the power sheet, show `[ RESTARTING ]` / `[ POWERING OFF ]` inside the tapped pill
      instead of a toast.

18. **[P1] Banner title overflows its card and the fallback icon adds a second red.** A long app
    name runs past the card's right edge at x=389, out to about x=410, cut mid-letter: "A VERY
    LONG APP NAM" (`banner/bn_long.png`). The icon-less banner draws a red dot while the
    stopwatch already shows its red live dot, so two reds are on screen. **Fix:**
    - `ui_draw_banner`: fit the label to `w-66-32` = 214 px with an ellipsis.
    - Use `DOT_ICON_BELL` in `C_WHITE` as the fallback, never red.

19. **[P2] The back swipe moves vertical sheets sideways.** A left-edge back swipe on the CC
    (`control/cc_backswipe.png`) and the face picker (`fp_cancel.png`) uses the rigid
    `TX_PUSH` slide. The sheet that came from the top exits to the right, and the face slides
    in from the left with -30% parallax. Power (no `SCR_HDRAG`) does it from any rightward drag.
    **Fix:** in `drag_decide`, for `TX_RISE`/`TX_DROP` entries, don't start the horizontal back
    gesture (or map it to the sheet's own axis). The picker should dissolve back, as it came
    in.

20. **[P2] Fling-to-dismiss brakes at release.** CC flick-close: the slider moves -79, -109 px
    per frame under the finger, then -67, -45 after release (`control/cc_flickclose_00*.png`,
    tracked with `tools/track.py`). A critically damped `SPRING_PAGE` loses about 40% of the
    speed in the first frame. **Fix:** when `finger_release` commits towards an off-screen end
    (pop/close), animate at `max(|v|, span/0.22 s)` with zero deceleration until off the glass.
    Keep the spring only for arrivals.

21. **[P2] Engine finger transitions and the brightness slider jump by the touch slop when they
    lock.** In `control/cc_close_00*.png`, the slider is static for frames 1-4, then jumps 44 px
    in frame 5, while the finger moved 22 px that frame. `finger_update` measures from the
    touch-down `P.y0`. The CC slider uses `in->dx` with no `sl_dx0`. The home pager already does
    this right (`grab_dx`). **Fix:**
    - In `finger_begin`/`nos_pull`, re-base `P.x0/P.y0` to the current finger point.
    - In `c_input` DRAG_START, store `C.sl_dx0 = in->dx` and use `in->dx - C.sl_dx0`.

22. **[P2] Transition dimming tints greys green.** `dim_rows` scales the 5/6/5 channels with
    truncation, so neutral greys gain green:
    - pressed card #181818 under the detail push becomes (8,12,8) (`notifs/nt_detail_open.png`
      frames 2-9);
    - launcher discs under the iris become (16,20,16) (`launcher/ln_tap_far_003.png`).

    **Fix:** after the multiply, if the source pixel was neutral (`G>>1 == R && B == R`), set
    `G = 2R`. Alternatively add the half-step before the shift:
    `d = ((d * k + 0x00400802) >> 5) & mask`.

23. **[P2] Swipe-to-dismiss.**
    - A 130 px drag (110 px after the slop) returns. The 110 px threshold gives no signal: no
      haptic and nothing revealed behind the card (`notifs/nt_swipe.png`).
    - After a flick, the card leaves at f345 but the gap starts closing at f348: a 3-frame black
      hole (`notifs/nt_swipe_flick.png`).
    - The count "7 NOTIFICATIONS" → "6" is a hard swap.

    **Fix:**
    - Send `FB_TICK` when `sw_x` crosses -110.
    - Grow a 44 px `C_SURF2` disc with a white dot "x" at x=380 as `sw_x` passes -40..-110.
    - Start the gap spring when the card is 50% off (`out.x < -CARD_W*0.5`).
    - Use `dot_text_morph` on the count.

24. **[P2] The banner is cramped and greedy.**
    - Its rounded top corners touch the bezel (top=38, w=312: the corner at y≈45 is about 1 px
      outside the glass).
    - The body is truncated to one 248 px line ("Ana: See you at 7...", `banner/bn_shown.png`)
      while the card has room for two.
    - It owns every touch with `y < 150`, including the bezel gap above it, so a pull from the
      top edge while a banner shows rubber-bands the banner 60 px and opens nothing
      (`banner/bn_pull.png`).

    **Fix:**
    - Set top=52 and w=300, which leaves a 13 px inset at the corner diagonal.
    - Wrap the body to 2 lines, with h=136.
    - `in_banner` should require `y >= banner top`.
    - A downward drag over 60 px should open the detail, as the tap does.

25. **[P2] The tile toggle-off animation turns the icon into a blob.** While the white disc
    shrinks (k 1→0.5) the ink stays black. Icon dots outside the shrinking disc become black on
    #1A1A1A and disappear: `cc_timing/cc_note.png` f235-f237 show a white blob where the Wi-Fi
    icon was. **Fix:** draw the icon twice: white ink, then black ink clipped to the disc
    (`gfx` clip circle). Or switch the ink at the first frame of the shrink and at the last
    frame of the grow.

26. **[P2] CC tiles: navigation looks like toggles, and the grid is tight.** Torch and Settings
    open screens, yet they render exactly like off toggles, so a user expects torch to toggle
    (`control/cc_open.png`). The 86 px discs sit on a 96 px pitch, leaving 10 px gaps, which is
    off the 8/16/24 scale and visually crowded. The brightness slider is continuous with no
    readout, while ticks fire at 10%. **Fix:**
    - Torch becomes a real toggle (long press opens the app).
    - Settings gets a ring style: `C_SURF2` disc plus 2 px `C_LINE2` outline, so it reads as an
      action.
    - Tile radius 40 with 16 px gaps.
    - Render the slider as 10 segments (`ui_segments`) with the % shown in the header note while
      dragging.

27. **[P2] The iris opens by deleting the tapped icon, and closes into a stale point.** From the
    launcher, the first visible effect of a tap is the heart icon going black: the app's
    background shows through at the icon centre, f301-f302 in `launcher/ln_tap_far.png`. A
    button from that app irises closed towards the old icon position (about x=96) over the
    face, not towards the button (`launcher/ln_app_btn.png` f400-f406). **Fix:**
    - Start the iris at `R0 = 36 px`, the icon disc radius, so the icon is the seed rather than
      a hole.
    - `nos_home()` sets `nos_iris_x/y = (GFX_W, GFX_CY)` when the popped entry is `TX_IRIS`.

28. **[P2] Notification cards: unbalanced padding and fixed height.**
    - Card 1 spans y 100-265 with the first ink at 122 and the last ink at 254: 22 px top, 11 px
      bottom (`notifs/nt_list_settled.png`, measured).
    - Body-less companion notifications keep the full 2-line height with about 60 px of empty
      surface (`banner2/b2_pull.png`, "Maya: Running late").

    **Fix:** `card_h() += 10`, and make the height per card (`52 + ascent + lines*body_lh + 26`)
    with a cached y prefix sum.

29. **[P2] Launcher: outer rows' names are cut by the bezel.** "Stopwat", "World cloc",
    "Navigat", "Recorde" are clipped at the right edge on rows ±2 (`launcher/ln_fling.png`
    f89, f93, f101, f110). The curve shift (`CURVE_K 0.78`) pushes them into the bezel. **Fix:**
    clip names to the chord with a 24 px fade-out, or cap `ix + NAME_DX + name_w` at
    `chord_right(cy) - 20`, sliding the row less.

30. **[P2] Long press on the face gives no feedback for 450 ms.** f26-f39 are identical, then
    the dissolve starts (`faces_power/fp_open.png`). **Fix:** at 150 ms of hold, start drawing
    the edit rim (from alpha 0 up to 3/8 at 450 ms) so the hold visibly charges.

31. **[P2] Power: arming has no timer and fills a large area with red.** "TAP AGAIN" is a
    236x72 solid `#D71921` fill (`faces_power/pw_armed.png`). The reference says never red
    fills on large areas, and the 3 s window expires silently. **Fix:**
    - Armed state: red outline plus red text "TAP TO POWER OFF".
    - Three red 6 px dots under it that go out one per second.

32. **[P2] The boot dissolve is a double exposure.** f40-f46 Bayer-blend "NOS." with the
    face's half-built digits, so both are unreadable for 7 frames (`boot_full/bf.png`). That is
    the crossfade the reference rules out. **Fix:**
    - At `T_POP`, collapse the wordmark dots into the face's lattice: same particles, 8 frames,
      `EASE_MECH`.
    - Or pop the boot screen with `TX_NONE` and let the face's own dot build-in be the reveal.

33. **[P3] Detail DISMISS gives no closure.** On return, the list simply has one fewer card
    (`notifs2/d_dismiss.png`). Collapse the card's slot with the gap spring after the pop.

34. **[P3] AOD entry is a one-frame cut** from the lit face to the grey AOD
    (`aod/aod_in.png` f26). Four quantised dim steps (`dim_rows` k 32→24→16→8) before the AOD
    draw would soften it.

35. **[P3] Toast copy is inconsistent.** "[ Restarting ]" is bracketed by the caller,
    "Opened on phone" and "Saved" are not. Make `ui_draw_toast` always add the `[ ]`, the
    `ui_state` idiom.

36. **[P3] The CC header note pops in** with no fade (`cc_timing/cc_note.png` f234). It fades
    out over 0.3 s, so mirror that: 3 frames in.

37. **[P3] The Haptics tile icon is an open palm,** which reads as "stop". Use a wave or a
    buzzing-disc dot icon.

## Resolved during this review (verify only)

- Page dots stuck at 3/8 on the face after any paging. The exponential fade with 1/8
  quantisation stalled, and the black capsule hid the bottom of "78%" (`home/face_unread.png`,
  `home/face_after_btn.png`, first build). The working-tree `sys_home.c` (linear ramp,
  DOTS_Y 448, capsule only off-face) fixes it (`home_v2/face_unread.png`). Keep the linear ramp.

## Already excellent (don't break)

- **Home pager:**
  - slop absorbed (`grab_dx`), 1:1 tracking;
  - the release velocity picks the page, capped at w0·d so it never overshoots
    (`home/pager_flick.png`, `ps/` displacement trace monotonic);
  - rubber band at both ends;
  - the alias trip makes page 5 → face one page of motion (`home/btn_to_face.png`);
  - a tick per page.
- **Launcher:** flings decay and land exactly on a row with a tick per row
  (`launcher/ln_fling.png`, 30 frames, smooth). The sprite8 LUT dimming keeps a full-motion
  frame at about 21 ms. The pressed state is visible.
- **Sheets:** grabbing a moving sheet mid-flight works cleanly, with no jump at the grab
  (`control/cc_grab.png` f306-f310). The pull follows the finger 1:1 once locked.
- **Banner:** spring in, flick-away with velocity (`banner/bn_flick.png`).
- **Notifications:** the swiped card follows the finger with a right-side rubber band. The gap
  closes on a critically damped spring. The empty state (ring dots filling, bell from the
  centre, `[ ALL CAUGHT UP ]`) is on-brand.
- **CC:** module-absent tiles are designed (`C_SURF` + `C_DIS` ink) and explain themselves
  ("NO LTE MODULE", `control/cc_absent_tap.png`).
- **Power:** the two-step destructive power-off, and the buttons in the thumb zone.
- **AOD:** sends 1 frame per minute with a pixel shift (`aod/aod_1min.png`; 2 frames sent in
  1818).
- **Damage check:** PASS on every script (home, control, notifs, banner, launcher,
  faces_power).
- **Cost:** 0 late frames anywhere. Worst is 24.4 ms (bus-bound full repaint). The face picker
  peaks at 23.4 ms with two faces drawn, the closest to budget.
