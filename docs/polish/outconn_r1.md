# NOS critique — outconn, round 1

Scope: Compass, Barometer, Navigate, Weather (+ wg_weather), Music (+ wg_music), Cellular / SMS,
Call, Find phone / Find watch, Recorder / Memos / Noise, Torch.
Sim: `build/nos_sim_crit_outconn.exe`. All evidence under `build/nos/critic/outconn/`
(scripts in `scripts/`, one folder per script). Paths below are relative to that folder.

## Verdict

The outdoor and connectivity apps have the Nothing look: true black, white dot-matrix heroes,
mono caps labels, a restrained single red. The motion foundations are good too: the
compass needle, the pagers and the scroll physics are smooth and never overshoot. Performance
is clean: 0 late frames in every script (worst frame 24.4 ms, bus-bound full repaints), and
`--damagecheck` passes on all 11 scripts. Behaviour and finish are behind the look. There are
three functional bugs:
- a GPS track stops recording as soon as you leave Navigate;
- the compass never draws its 300° and 330° labels;
- Recalibrate on the compass does nothing.

There is also a cluster of P1 flaws a user would notice: a one-frame title flash on track
change, an inbox that blanks and rebuilds after every back, pressed states you can't see,
labels colliding with rings and ripples, and amber used against the palette. Below the bug
fixes, the work is consistency. Absent states, pressed states, value changes (morph or
hard cut), centring of heroes with a degree mark, and widget-to-app continuity are each
done three different ways across these ten apps.

## Issues (most severe first)

1. **[P0] Navigate — a recording track dies when you leave the app.** `nav_leave()` calls
   `hal_gps_enable(0)`, and nothing records in the background. Repro: start a track, record
   20 s (23 M), go home for 30 s, reopen. The screen shows the sky plot `[ SEARCHING 0/4 ]`
   with `● REC`. 7 s later it reads `REC 00:57  49 M`: the timer counted the 30 s away, but
   the distance only grew by the straight-line jump. A single button press (to check the time)
   ruins a walk.
   Evidence: `nav_bg/nb_before_leave.png`, `nav_bg/nb_reopen_1s.png`, `nav_bg/nb_reopen_7s.png`.
   Fix: move the track recorder into a service (`svc_track_*`, like the timers in `svc.h`).
   Keep the GPS enabled and exempt from `NOS_EXT_IDLE_MS` while `rec` is set, and add points
   from the service tick. The app only renders it. Show a REC indicator on the face or
   control centre while it is running.

2. **[P0] Compass dial — the 300° and 330° labels are never drawn.** In `dial_draw` the
   numeral gate is `g = (b*1.4 - k*6/72*0.9)*3 - 1`, and numerals need `g >= 1`. At `b = 1`
   that gives k=10 → 0.95 and k=11 → 0.725, so both numerals never appear. The upper-left
   quadrant between W and N is always bare.
   Evidence: `compass/c_dial_30.png`, `compass2/c2_after_cal.png` (no 300/330 between W and
   N), `compass/c_turn_wrap.png` frames 7-19 (heading 340: the 300/330 region is empty).
   Fix: use 1.75 instead of 1.4 for the label sweep. k=11 then gives
   `(1.75-0.825)*3-1 = 1.78`. Or simply force `g = 1` when `b >= 1`.

3. **[P0] Compass — Recalibrate does nothing.** Tap the dial, then tap CALIBRATE. The screen
   jumps straight to `[ CALIBRATED ]` with the full ring, with no figure-8 and no progress:
   `if (p >= 0.95f || C.mag.calibrated)` is already true because the old calibration flag
   is still set.
   Evidence: `compass/c_cal_pressed.png` (taken one frame after the tap: full ring,
   CALIBRATED), `compass/c_level_enter.png`.
   Fix: on a manual recalibration set `C.recal = 1`, ignore `mag.calibrated` until
   `cal_progress` has dropped below 0.1 and climbed back to ≥ 0.95. Or have
   `hal_mag_calibrate(1)` clear the calibrated flag.

4. **[P1] Music — the new title flashes at full white for one frame on Next.** Measured on
   the title band (x 150-320, y 170-200):
   - mn_002: `#FFFFFF` (the new title, drawn once at full intensity)
   - mn_003: `#414041`
   - mn_005: `#838183`
   - mn_006: `#BDBEBD`
   - mn_008: `#FFFFFF`

   It reads as a flicker. The same thing is visible in the thumbnails: `music/m_next.png`
   frames 2→3.
   Evidence: `music_next/mn_000..011.png`.
   Fix: set `M.txt = 0` in the same tick that detects the track change, before the next
   draw. Or start the fade on the frame the musicinfo arrives. Never draw new text at
   `txt = 1`.

5. **[P1] Messages inbox — the list blanks and rebuilds after every back.** After a reply is
   sent, the reader pops and the inbox slides in fully drawn (f339-f343). At f344 every card
   vanishes, leaving only the title, and the rows then re-stagger in (f345-f348).
   `inbox_enter()` resets `I.t_enter` when the inbox is revealed by a pop.
   Evidence: `cell2/c2_sent.png` frames 50-59.
   Fix: only reset `t_enter` (and the stagger) on a fresh push. When the inbox is revealed by
   a pop, keep `st = 1`.

6. **[P1] Inbox rows have no visible pressed state.** The card fill goes from `#101010` to
   `#181818`, which is imperceptible on the panel.
   Evidence: `cell/c_row_pressed.png` vs `cell/c_inbox.png`, pixel (100,190).
   Fix: pressed fill `C_LINE2` `#333`, or `C_SEC` with dark ink as `wg_music` does (that one
   reads well: `music/m_wg_toggle.png` f0-f2). Apply the same to the baro ± buttons and
   Calibrate (`#181818`→`#292829`, `baro/b_plus_pressed.png`), the reply pills and the memo
   rows.

7. **[P1] Inbox — cards near the bottom are truncated to nothing.** The third card shrinks to
   x 124-342 with the circle (`ui_row_span`). It shows `+9199005...` and the preview `Di...`.
   Left text edges jump between rows: x=147, 86, 125 in `cell/c_inbox_scrolled.png`, so the
   eye has to hunt for each line start.
   Evidence: `cell/c_inbox.png`, `cell/c_inbox_scrolled.png`.
   Fix:
   - Use a constant card width (x 53-413, 360 px) for the row in the centre band and scale
     cards down towards the edges (watchOS fisheye), rather than cropping text.
   - Keep the text left edge constant at card x + 40.
   - Give the sender the full first line and drop the time onto the right of line 1, so the
     preview gets the whole second line.

8. **[P1] Barometer — the back swipe in the sea-level picker exits the whole app.** The side
   button cancels only the picker. The swipe pops Barometer, so `expect top BAROMETER` fails
   with HOME. Two cancels with two outcomes, and an edit-in-progress is thrown away silently.
   Evidence: `baro_back/` (log: `EXPECT FAIL top: got 'HOME'`).
   Fix: make the picker a pushed screen (`scr_baro_cal`, `TX_PUSH`) so back = cancel and the
   engine owns it. That also gives the picker a real transition instead of today's hard cut
   (`baro/b_cal_open.png` f198: all chrome pops in at once).

9. **[P1] Barometer picker — "Use GPS" appears mid-use and moves Set from under the
   thumb.** Set is a single centred pill (x 135-331) until the GPS gets a 3D fix about 5.5 s
   after the picker opens. Then the layout snaps to `USE GPS` (x 78-228) and `SET`
   (x 238-388). A tap aimed at Set's old centre now hits the gap or USE GPS.
   Evidence: `baro/b_cal_long.png` vs `baro/b_cal_gps.png`.
   Fix: always lay out two buttons. Show USE GPS disabled (`C_SURF` fill, `C_DIS` ink) until
   the fix, and enable it in place with a 150 ms ink fade.

10. **[P1] Navigate sky plot — `● REC` collides with the `N` label.** The REC dot and label
    are at (189,98) and y 104, and the sky-plot `N` is at y 108. They overprint.
    Evidence: `nav_bg/nb_reopen_1s.png`.
    Fix: put REC in the header slot the data pages use (y 64-70, centred). Move `NAVIGATE`
    to the label line, or drop it while recording.

11. **[P1] Amber is used against the palette.** `C_AMBER` is used for `[ HEARING RISK ]` and
    the >85 dB meter dots on Noise, and for `STORM WARNING` in Barometer (code,
    `out_baro.c:251`). §2 lists only greys + one red. Over-limit and alerts are red's job.
    Evidence: `memo/r_noise_loud.png`.
    Fix: draw `[ HEARING RISK ]` and the over-85 meter dots in `C_RED`; they are the
    screen's only red. Same for `STORM WARNING`. (Storm can't be triggered in the sim: the
    synthetic front is 10 h before boot. Please add a `set storm 1` sim key so it can be
    reviewed.)

12. **[P1] Compass Level — the tilt readout touches the outer ring.** The ring bottom is at
    y≈377 and the readout digits start at y=378. That is a gap of 1-3 px, against an 8 px
    minimum.
    Evidence: `compass2/z_level_readout.png`, `compass2/c2_level_tilt.png`,
    `compass2/c2_level_bigtilt.png`.
    Fix: readout at y 392 (and `[ FLAT ]` at 398 already matches), or outer ring r 150 → 140.

13. **[P1] Noise, mic absent — the body copy is truncated:** `Add the mic strap to mea...`.
    The designed empty state ends in an ellipsis.
    Evidence: `memo/r_noise_nomic.png`.
    Fix: wrap to 2 lines (`ui_text_wrap`, 330 px, as Compass/Baro/Nav do) or shorten to "Add
    the mic strap to measure noise" in 2 lines. The icon there is a speaker; use the mic
    icon (the missing module is the mic).

14. **[P1] Find watch — the text sits on top of full-brightness ripples.** White dot rings
    sweep through `FIND WATCH` (y 64) and `Tap to stop` (y 396). The instruction is
    unreadable in about half the frames.
    Evidence: `find/fw_ringing.png`, `find/fw_ripple.png`.
    Fix: draw a `C_BG` pill (220×44, r 22) behind each text line, or skip ripple dots with
    |y-64| < 22 or |y-396| < 22.

15. **[P2] Absent / not-connected states are inconsistent across the apps.**
    - Compass/Baro/Nav use: dot icon y 176, "Not connected" (title size) y 276, a 2-line
      body, and a part-number label y 404 (`compass/c_absent.png`).
    - Cellular uses "Module not connected" in body size, then "NO PHONE NEEDED" (an unclear
      label), and stops at y 352 (`cell2/c2_absent.png`).
    - Recorder uses "Module not connected" in body size (`memo/r_nomic.png`).

    The part numbers (`QMC5883L  STRAP`, `BME280  STRAP`, `M10 GPS  STRAP`) are engineering
    names, not user words.
    Fix: one shared `ui_absent(title, icon, line)` with the Compass layout, and a bottom
    label such as `COMPASS MODULE` / `CLIMATE MODULE` / `GPS MODULE` / `LTE MODULE` /
    `MIC MODULE`.

16. **[P2] Heroes with a degree mark sit about 20 px right of centre.** The digits are
    centred and the mark hangs right, so the whole group is off-centre. Measured bboxes:
    - Barometer LOCAL `26.0°`: x 148-362, centre 255 (+22). Evidence: `baro/b_local.png`.
    - Compass `61°`: x 190-315, centre 252.5 (+19.5). Evidence: `compass2/c2_after_cal.png`.
    - wg_weather `24°`: x 190-317, centre 253 (+20). Evidence: `wx/w_widget.png`.

    Compare the pressure hero without a mark: 134-331, centre 232.5, correct.
    Fix: shift the digits left by half the mark's advance: `cx - 0.5*(pitch*1.3 + markw)`,
    about -16 px at pitch 8 and -18 px at pitch 9. Then centre the composite.

17. **[P2] Barometer chart — the "now" column is a bar of 7 red dots** (224 red px). That is
    a red fill, not a red dot. Evidence: `baro/b_pressure.png` (x 340-352).
    Fix: draw the now column in `C_PRI` with only its top lit dot `C_RED`.

18. **[P2] Weather never goes idle.** The icon loop repaints its box every frame forever:
    2.9 ms each frame, `sent` = every frame in `wx/summary.txt`, section loop 120/120. Nothing
    plays its weather animations on open or tap.
    Fix: play 2 loops (8 s), then hold the loop's rest frame. Restart on tap and on reopen.
    The same applies to wg_weather.

19. **[P2] Weather icons are inconsistent and partly sliced.**
    - Partly cloudy: the sun disc is cut by a diagonal edge into half-dots, and the sun
      (≈4 px pitch) and the cloud (≈7 px pitch) are on two different grids
      (`wx_c1/z_partly.png`). Fix: occlude whole dots (skip a sun dot whose centre is inside
      the cloud mask + 1 pitch) and use one pitch for the whole icon.
    - Snow: the snow is four-dot diagonal streaks, the same shape as wind-driven rain
      (`wx_c5/icon_strip.png`). Fix: slower fall (≤ 1 px/frame), ±2 px sideways sway per
      flake, and flakes as 5-dot `+` clusters.
    - Rain (`wx_c3/z_rain.png`) and all cloud icons have a 3-dot notch in the cloud's bottom
      row (x≈190-240) that reads as a bite.
    - Hourly row: the icons are a separate outline, single-size family
      (`wx/w_after_flick1.png`) and don't match the filled multi-size hero icons.
      Fix: use `wx_icon()` at scale 0.3 with the loop frozen.

20. **[P2] Weather — the hourly and 5-day sections build in late while you scroll.** The
    `NEXT 12 HOURS` section is on screen but mostly empty for about 11 frames as the cells
    stagger in one by one (f164-f175).
    Evidence: `wx/w_scroll_flick.png` frames 6-17.
    Fix: start a section's build when it is 1 page below the viewport, or cap the stagger at
    6 frames total (2 rows × 1 frame per column pair).

21. **[P2] The weather empty state disagrees between app and widget.** With the phone
    connected but no feed, the app shows `[ UPDATING ]` forever (`wx_cnone/icon_now.png`).
    The widget shows `[ NO WEATHER ] CONNECT YOUR PHONE` (`wxn/wxr_widget.png`), which is
    false: the phone is connected.
    Fix: one state function. Connected → `[ UPDATING ]` for 30 s, then
    `[ NO DATA ] / OPEN GADGETBRIDGE`. Not connected → `[ NO WEATHER ] / CONNECT YOUR PHONE`.

22. **[P2] Widget → app has no visual continuity.**
    - Weather: the widget stacks icon (y 168) over temperature (y 262-332). The app sets
      them side by side, with the temperature at y 145-220. On tap, everything rebuilds
      from dots in new places (`wx/w_wg_tap.png`).
    - Music: the widget play disc is r 60 at y 212, the app's is r 54 at y 308. The disc
      collapses to a dot and regrows elsewhere (`music/m_wg_open.png` f868-f873).

    Fix: open from the widget with the hero kept in place (draw the app's first frame with
    the widget's hero position and move it with `EASE_STANDARD` over 300 ms). Or make the
    widget layout equal the app's first screen.
    The weather widget also has no pressed state: frames 0-1 are identical in
    `wx/w_wg_tap.png`.

23. **[P2] Call — the caller name changes position and truncation in every state, and
    Decline flashes the ended layout.**
    - Incoming: `Ana Lucia Fernandes-...` at y 170.
    - Active: `Ana Lucia Fernande...` at y 118.
    - Ended: `Ana Lucia Fernandes-R...` at about y 160.

    All three transitions are hard cuts (`call/k_incoming.png`, `call/k_active_61s.png`,
    `call/k_ended.png`). On Decline, `[ CALL ENDED ]` is drawn for exactly one frame
    (f2191) before the screen drops away (`call/k_decline.png`).
    Fix: wrap the name to 2 lines at a fixed y (title font, 340 px). Move only the elements
    that change: the buttons morph, decline+accept → end with a 250 ms EASE_STANDARD slide.
    On decline, pop straight from incoming.

24. **[P2] Call — the comet runs over the button labels.** The ring is at r 214 and the
    labels are at y 404-410. The comet dots overprint `ACCEPT` (f170-f172) and `DECLINE`
    (f177-f179).
    Evidence: `call/k_comet.png` frames 27-29, 34-36.
    Fix: labels at y 392, or skip comet dots in the 120°-240° arc while incoming.
    Also replace `ON PHONE` on the incoming screen (it reads as "already on the phone")
    with `ANSWERS ON PHONE`.

25. **[P2] Find phone — the ripples reach the title.** RIP_R1 = 160 around cy 206 puts the
    outer ring at y≈46, through `FIND PHONE` (y 64). Evidence: `find/f_ringing.png`.
    Fix: RIP_R1 = 118, or clip ripple dots above y 92.

26. **[P2] Compass — the Level mode is undiscoverable, and FLAT fires before the bubble
    arrives.**
    - Long press toggles Level with no hint and no hold feedback: 20 frames of nothing
      (`compass2/c2_level_enter.png`). Fix: in the details state, add a second label
      `HOLD FOR LEVEL` under FIELD. Better: make Level a second pager page with page dots.
    - `[ FLAT ]` and the white target ring switch on from the raw tilt while the bubble is
      still overlapping the ring edge (`compass2/c2_level_move.png` f78-f79). Fix: drive
      `level_ok` from the displayed bubble offset (`hypot(bub_x,bub_y) < 6`).

27. **[P2] Barometer picker — the value doesn't follow the finger and doesn't scale.**
    - The number hard-cuts (1013.3→1013.6 in one frame, f254). Drag is 8 px per 0.1 hPa
      with no acceleration, so a 5 hPa correction is a full-screen drag
      (`baro/b_cal_drag.png`).
    - Long-press on ± gives one ±1.0 step and no repeat: 1015.3→1016.3 only
      (`baro/b_cal_long.png`).

    Fix: velocity-scaled steps (0.1 / 0.5 / 1.0 hPa at <300 / <900 / ≥900 px/s), an
    auto-repeat of 8 Hz after a 400 ms hold, and a 1-frame-stagger digit morph on change.

28. **[P2] Navigate — the fix arrival is a hard cut, and the satellite count is
    meaningless.**
    - Sky plot → NOW page in one frame (f120→f121). Only the speed hero builds. The
      coordinates and the white `START TRACK` pill pop in under the thumb
      (`nav2/n2_fix.png`).
    - The counter reads `0/14` until the instant of the fix, so it never shows progress.

    Fix: light satellites white as they become used, then build the NOW rows with a 2-frame
    stagger. Fade the pill in last (≥ 150 ms after the hero) so it isn't hit by accident.

29. **[P2] Navigate — Stop ends the track on one tap, with no summary.** One tap on Stop
    saves and resets, and shows only `[ SAVED 0001 ]` for 2.5 s (`nav/n_stop.png`).
    Fix: hold-to-stop (600 ms, with a dot ring filling around the pill), then
    `[ SAVED ]` + `1.2 KM · 14:05` for 3 s.
    `START TRACK` also nearly fills its 176 px pill (12 px side padding,
    `nav/n_fix_3d.png`): widen it to 208 px.

30. **[P2] Values hard-cut where the spec asks for a morph, and Noise morphs all the time.**
    - Speed (`nav/n_stop.png` 5.6→5.5), back-to-start distance and the compass readout jump.
    - Noise updates about 3 Hz with a 0.28 s morph, so the digits are half-built in about
      40% of frames (`memo/r_noise_live.png` f584-f587, f592-f594, f601-f602).

    Fix: speed and distance get the 0.28 s `dot_text_morph`. Noise: refresh the hero at
    1 Hz (the meter can stay fast) or shorten its morph to 5 frames.

31. **[P2] Music — an empty track shows a fake player.** A musicinfo with an empty track plus
    `state stop` renders `Unknown track` with a live play button and `0:00 • --:--`. The
    designed `[ NOTHING PLAYING ]` state never appears.
    Evidence: `music/m_empty.png`.
    Fix: `have = track[0] || artist[0]`, and fall through to the empty state otherwise.
    The not-connected screen has no app title (`music/m_notconn.png`): add
    `ui_title("MUSIC")`.

32. **[P2] Music — uneven vertical rhythm.** EQ bottom (130) → title (176) is 46 px, but
    artist bottom (233) → control tops (254) is 21 px, so the controls crowd the artist.
    Evidence: `music/m_playing.png`.
    Fix: move the EQ to y 96 and the title/artist up 8 px (TITLE_Y 188, ARTIST_Y 224). That
    gives about 30 px above the controls.

33. **[P2] RGB565 dimming of dark greys gives green casts.** Mid-fade card fills come out
    `#080C08` and `#000400` (inbox push frames 6-8; baro chart pixels). Cards visibly flash
    green in `cell/c_inbox_push.png` f235-f241.
    Fix: in `ui_dim` for neutral colours, quantise one grey level and write R=B=level,
    G=level<<1 (5-6-5 neutral), or step through the grey ladder (#000/#080808/#101010/…).

34. **[P2] Torch — a dead black beat on entry, and an illegible gauge in red mode.**
    - Entry: the iris ends on 3 black frames (f4-f6) before the dither dissolve starts
      (`torch/t_entry.png`). Fix: start the dissolve at `t_enter` or open Torch with
      `TX_NONE`.
    - Red mode: the gauge is `#100000` lit vs `#500000` unlit on `#780000`, and the `80%`
      label is the same dark red (`torch2/strip.png`). Fix: lit `#000000`, unlit `#3C0000`,
      label `#000000`.
    - The hint teaches only the tap (`TAP FOR RED LIGHT`). After it fades, show
      `DRAG TO DIM` once.

35. **[P2] Memos — no-speaker state and ambiguous metadata.**
    - With no speaker, every row keeps an active play button. The only signal is a crowded
      title, `MEMOS  /  NO SPEAKER` (`memo/r_memos_nospk.png`). Fix: draw play icons in
      `C_DIS` on `C_SURF`, make the row tap give `FB_ERROR`, and put `[ NO SPEAKER ]` as the
      list footer.
    - Rows read `00:06  10:09`: duration and clock time in the same format, unlabelled
      (`memo/r_memos.png`). Fix: `6 S · 10:09` / `2:07 · 09:54`.
    - While playing, the metadata line is replaced by the dot bar, so elapsed time
      disappears (`memo/r_playing.png`).

36. **[P2] Navigate — losing the fix throws away all context.** On a lost fix the page drops
    back to the bare sky plot (`nav/n_lost_fix.png`).
    Fix: keep the NOW page with the last values in `C_DIS` and the header `[ NO FIX ]  0:12
    AGO`. Return to the sky plot only after 30 s without a fix.

37. **[P2] Cellular status screen — redundant copy and an ambiguous radio icon.** `[ OFF ]` +
    `Cellular is off` say the same thing twice. The radio button uses the same signal-bars
    icon as the hero, so it doesn't read as power, and its label `RADIO` is `#666`
    (`cell/c_off.png`).
    Fix: body `Tap RADIO to turn on`, a power glyph on the button, and the label in `C_SEC`.

38. **[P3] Compass settle tail repaints without visible change.** f47, f49, f51
    (`compass/c_turn_slow.png`) cost 20.5 ms each while the display already reads 75°.
    Quantise `shown` to 0.5° (draw resolution is 1°).

39. **[P3] Compass details label `FIELD 48 uT`** has a lowercase `u` in an all-caps mono label
    (`compass/c_details.png`). Use `48 µT` if the font has µ, else `FIELD 48 UT`.

40. **[P3] Compass lubber** dots sit 5 px from the glass edge (y 5-19, `compass/z_w_lubber.png`).
    Move it to y 12 (8 px inset) so it clears the panel's mask tolerance.

41. **[P3] Copy widows.**
    - `Move your wrist in a / figure 8` (`compass2/c2_cal_mid.png`) → break as
      `Move your wrist / in a figure 8`.
    - `Start one on the first / page` (`nav/n_track_empty.png`) → `Start one on the NOW
      page` in 280 px, or reword to fit one line.

42. **[P3] Smaller details.**
    - Temperatures have no unit: `26.0°`, `24°` (add `C` at label size after the mark, or a
      `°C` label).
    - The humidity bar draws a shrunken partial 12th dot (`baro/b_local.png` x≈253). Round
      to whole dots.
    - The marquee's right edge leaves a sliced glyph stump (`music/z_mq_rest.png`). Clip at
      a glyph boundary or extend the last fade step to 0.
    - Find phone offline: a tap gives no visual feedback (`find/f_offline_tap.png`). Add
      `FB_ERROR` and a 150 ms disc press.
    - `WRIST 26°` beside an 18° air temperature (`wxr/wxr_app.png`) will confuse; call it
      `STRAP 26°`.
    - The recorder silently ignores horizontal swipes while recording. Rubber-band 20 px
      and give `FB_LIMIT`.
    - `memo_leave()` stops and saves a recording on any button press. That is consider-worthy
      for an accidental press; a recording service would match the timers.

Not reviewed: the Storm warning (no sim trigger; see #11) and SD-card failure states
(`NO SD CARD`).

## Already excellent (don't break these)

- **Performance.** 0 late frames across all 20+ scripts; worst frames are 24.4 ms full
  repaints on entry. Pagers cost 6-10 ms while moving (subview damage). `--damagecheck`
  PASSes on compass2, baro, nav2, music, music_next, cell2, call, find, memo, torch and
  wx_full.
- **Compass needle.** Damped, monotonic, no overshoot; it takes the short way round (75°→340°
  goes through 0, `compass/c_turn_wrap.png`) and settles exactly. The dial build sweep from
  north is lovely.
- **Pagers** (baro / nav / memo). 1:1 tracking, rubber band, velocity hand-off, monotonic
  settle (page-dot centroid 223.3→232.5 with no reversal, `pager/`).
- **Weather scroll.** iOS decay, rubber band and settle (`wx/w_rubber.png`), and the
  scrollbar fade.
- **Weather loops.** The sun, storm-flash, wind and night loops are subtle and dot-native.
- **Red discipline.**
  - The REC dot turns white on the TRACK page where the red "you" dot takes over
    (`nav/n_track_90s.png`).
  - The recorder's red dot morphs into a rounded square in 4 frames (`memo/r_rec_start.png`).
  - Decline/End are the call's single red.
- **Music.**
  - The play↔pause dot morph.
  - The EQ lies flat on pause.
  - The marquee holds 2 s, then moves at an exact 1 px/frame with a stepped fade.
  - The volume pill swap with the timeline.
  - The widget's grey pressed disc.
- **Feedback moments.**
  - The Cellular radio button fills with a white disc growing from its centre
    (`cell/c_power_on.png`).
  - The signal-bar sweep while starting.
  - Find phone's ripple grey-ladder fade-out after stop (`find/f_stop.png`).
  - The `[ SENT ]` check build.
- **Torch.** The ordered-dither dissolves (3 frames, never a hard cut to white) and the
  10%-detent drag gauge.
- **Navigate back-to-start.** The arrow built from dots, with the tip as the biggest dot, and
  the track view (start ring, red you, scale bar).
