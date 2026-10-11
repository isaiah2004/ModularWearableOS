# NOS critique: outconn, round 2

Scope: Compass, Barometer (incl. the new sea-level picker screen and storm), Navigate (incl.
the track service), Weather (+ wg_weather), Music (+ wg_music), Cellular / SMS, Call, Find
phone / Find watch, Recorder / Memos / Noise, Torch. I also checked these screens against the
§8 standards in docs/NOS_DESIGN.md.
Sim: `build/nos_sim_crit_outconn.exe`, rebuilt at 5e1bd36. Evidence is under
`build/nos/critic/outconn/r2/`, one folder per script; paths below are relative to that
folder. Scripts are in `build/nos/critic/outconn/scripts/`: all 26 round-1 scripts plus 10 new
ones (`r2_*`). Crops are `z_*.png`.

## Verdict

This round fixed a lot, and most of the fixes are good ones. All three P0s are fixed:
- Tracks survive leaving the app.
- The compass draws 300 and 330.
- Recalibrate works.

The baro picker is now a real pushed screen, with the right back behaviour, a disabled-in-place
USE GPS, and hold-repeat. Lost-fix handling, absent states, weather idling and the weather
widget-to-app continuity all went from weak to good.

Performance is still clean:
- 0 late frames across 45 runs.
- `--damagecheck` PASS on 21 scripts (14,040 frames, damage-tracked == full repaint).

Some fixes are mediocre, and the new work brought new flaws:
- The track service records in the background with no indicator anywhere outside Navigate.
- Every call end prints the `[ CALL ENDED ]` label in black across the red End disc for a frame.
- Several fixes swap one rule break for another. Amber became red text. Ripples behind text
  became sliced half-dots.

§8 compliance is uneven. Units are the clearest case: `hPa`, `dB`, `855 m` keep their case,
but `KM/H`, `M`, `METRES`, `DBM`, `7 S` and `/ 3H` don't. State changes are another:
Navigate's Stop and Weather's data arrival are still hard cuts. Red is also used as text and
as area fills, which §8 forbids. The scorecard: 30 of 42 round-1 items are fixed (four with
small residue), 11 are partly fixed, and #42 is mostly fixed. There are 2 new P1s and 10 new
P2s.

## Round-1 verification

| # | Round-1 issue | Status | Evidence / note |
|---|---|---|---|
| 1 | P0 Nav track dies on leave | **Fixed** | `nav_bg/nb_before_leave.png` 23 M @0:20 → `nb_reopen_1s.png` 64 M @0:51 → `nb_reopen_7s.png` 70 M @0:57. New problem: no REC indicator outside the app (new #2). |
| 2 | P0 Compass 300/330 labels missing | **Fixed** | `compass/c_dial_30.png` |
| 3 | P0 Compass recal no-op | **Fixed** | Restarts from 0%: `compass/c_cal_pressed.png`, `c_recal.png`, `c_cal_late.png` |
| 4 | P1 Music next-title white flash | **Fixed** (residue) | No white frame: `music_next/mn_002.png` title band is all #000000, then #414041 → #838183 → #BDBEBD → #FFFFFF. The old title hard-cuts out to a blank frame first (new #20). |
| 5 | P1 Inbox blanks/rebuilds after pop | **Fixed** | Revealed fully built, no re-stagger: `cell2/c2_sent.png` f327-f348, `r2_cellpop/sp_000..009.png` |
| 6 | P1 Pressed states invisible | **Fixed** (residue) | Inbox #181818 → #313031 (`cell/c_row_pressed.png`). Baro ± / Set, reply chips and memo rows also visible. Quick taps hold the pressed state 2 frames, not the ≥ 3 that §8 asks for (new #13). |
| 7 | P1 Inbox cards truncated / ragged edges | **Fixed** | The team listed this as not fixed. It is fixed: cards are now a fixed 52-414 width, r40, 90 px pitch, with a constant text edge (`cell/c_inbox.png`, `c_inbox_scrolled.png`). That is what §8 asks for, so no fisheye is needed. |
| 8 | P1 Baro picker back exits app | **Fixed** | Pushed `SEA LEVEL` screen. Back returns to BAROMETER (`baro_back/bb_after_back.png`). |
| 9 | P1 Use GPS appears mid-use | **Fixed** | Pair always laid out, USE GPS disabled until fix (`r2_baro/k_picker.png` vs `k_gps_ready.png`) |
| 10 | P1 Sky plot REC collides with N | **Fixed** | REC in header y 64 (`r2_nav/n_nofix_sky.png`) |
| 11 | P1 Amber | **Partly fixed** | Amber is gone. It was replaced by red text, which §8 also forbids: `[ STORM WARNING ]` in red (`storm/s_storm.png`); Noise has a red meter dot plus a red `[ HEARING RISK ]` label (`memo/r_noise_loud.png`). See new #4. |
| 12 | P1 Level readout touches ring | **Fixed** | `compass2/c2_level_tilt.png` |
| 13 | P1 Noise no-mic truncated + speaker icon | **Fixed** | `memo/r_noise_nomic.png`: mic icon, 2-line body, `MIC MODULE` |
| 14 | P1 Find watch text over ripples | **Partly fixed** | The text is legible now, but the knockout is a rectangle clip that slices ripple dots into half-discs (`find/z_fw_title.png`, `z_fw_hint.png`). See new #9. |
| 15 | P2 Absent states inconsistent | **Fixed** | `ui_absent` + `<NAME> MODULE` everywhere (`compass/c_absent.png`, `cell2/c2_absent.png`, `memo/r_nomic.png`). Phone-absent copy matches §8 (`music/m_notconn.png`, `find/f_offline.png`). Weather's empty state is the exception (new #8). |
| 16 | P2 Degree heroes off-centre | **Fixed** | Baro LOCAL 134-323, centre 228.5. Compass 166-291, centre 228.5. |
| 17 | P2 Baro now column 7 red dots | **Fixed** | 44 red px, top dot only (`baro/b_pressure.png`) |
| 18 | P2 Weather never idle | **Fixed** | `r2_wx_wg`: widget section `wgidle` 60/60 frames idle, app `appidle` 60/60 idle. A tap on the icon replays. |
| 19 | P2 Weather icons inconsistent | **Fixed** (residue) | One grid. Partly-cloudy is one figure, snow is `+` flakes, no cloud notch, hourly uses the hero icons (`z_wx_icons.png`, `wx/w_after_flick1.png`). The crescent and wind icons are weak (new #21). |
| 20 | P2 Hourly builds late in scroll | **Partly fixed** | Down to 5 columns, but icons still fade in over ~8 frames while the section is on screen (`wx/w_scroll_flick.png` f163-f170) |
| 21 | P2 Weather empty app vs widget | **Fixed** | Both show `[ UPDATING ]` (`wx_cnone/icon_now.png`, `wxn/wxr_widget.png`). The state itself is too thin (new #8). |
| 22 | P2 Widget → app continuity | **Partly fixed** | Weather is excellent: same layout, pressed card, facts fade in (`wx/w_wg_tap.png`). Music: the disc moves, but the title dissolves for 3 frames and pops in, and a stale pressed grey shows on return (new #10). |
| 23 | P2 Call name position / truncation | **Fixed** | Long name wraps at the hyphen, same block in every state. Decline goes straight to the face (`call/k_incoming.png`, `k_active_61s.png`, `k_decline.png`). |
| 24 | P2 Comet over labels, ON PHONE copy | **Partly fixed** | No overlap now, but min clearance is 11.3 px to ACCEPT, 11.7 px to DECLINE and 14.6 px to the name (`r2_comet/cm_031`, `cm_038`, `cm_000`). The copy is now `ANSWERS ON PHONE`, which is good. |
| 25 | P2 Find phone ripples reach title | **Fixed** | `find/f_ringing.png` |
| 26 | P2 Level undiscoverable, FLAT early | **Partly fixed** | `HOLD FOR LEVEL` hint in details. FLAT waits for the bubble. Long-press still has no hold feedback, and `0.0°` shows while the bubble is off-centre (new #16). |
| 27 | P2 Baro picker drag / accel / repeat | **Fixed** | Repeat ~8 Hz after 400 ms with morph (`r2_baro/k_hold.png`), 1:1 slow drag (`r2_baro/z_drag.png`) |
| 28 | P2 Nav fix hard cut + count | **Partly fixed** | No hard cut, but the transition takes ~13 frames (433 ms) and passes through a near-blank f129 (`nav2/n2_fix.png`). §8 says 150-300 ms. |
| 29 | P2 Nav stop one tap, no summary | **Partly fixed** | Hold-to-stop with a dot ring and `HOLD TO STOP` taught on tap (`nav/n_stop.png`, `r2_nav/n_hold.png`). `[ SAVED ] 45 M 00:37` summary. STOP → SAVED → START are hard cuts (new #6). |
| 30 | P2 Values hard-cut / noise always morphing | **Partly fixed** | Noise holds and morphs only on change (`memo/r_noise_live.png`). Distance jumps 0 → 51 M on refix, and the dimmed speed still morphs after fix loss (new #15). |
| 31 | P2 Music empty fake player / no title | **Fixed** | `music/m_empty.png` `Nothing playing`; `m_notconn.png` titled |
| 32 | P2 Music vertical rhythm | **Fixed** | `music/m_playing.png`: gaps 33 / 27 / 26 px |
| 33 | P2 RGB565 green casts | **Fixed** | Surfaces are neutral (#181818, #313031). What remains is +3/+4 G on anti-aliased text edges only, 0.7% of pixels. |
| 34 | P2 Torch black beat, red gauge, hint | **Partly fixed** | No black beat (`torch/t_entry.png`). `DRAG TO DIM` hint added. Red gauge unchanged: black on #7B0000, 1.8:1 (`torch/t_red_level.png`). |
| 35 | P2 Memos no-speaker / metadata / elapsed | **Fixed** | Dimmed play icons + `NO SPEAKER MODULE` toast (`r2_recleave/nt.png`). `7 S · 10:09`. Elapsed shown while playing (`memo/r_playing.png`). Unit case: new #5. |
| 36 | P2 Nav lost fix drops context | **Fixed** | `[ NO FIX ] 0:11 AGO` over dimmed values; sky plot after 30 s (`r2_nav/n_nofix_12s.png`, `n_nofix_sky.png`) |
| 37 | P2 Cell OFF copy / icon / label | **Fixed** (residue) | Power glyph, labels #9C999C (`cell/c_off.png`). The sentence-case hint now conflicts with §8 (new #24). |
| 38 | P3 Compass settle tail | **Fixed** | Idle from f46 (`compass/c_turn_slow.png`) |
| 39 | P3 `FIELD 48 uT` | **Fixed** | §8 sanctions `48 uT` |
| 40 | P3 Lubber 5 px from edge | **Fixed** | y ≈ 10-20 |
| 41 | P3 Copy widows | **Partly fixed** | Compass fixed (`Move your wrist / in a figure 8`). Nav still widows `Start one on the NOW / page` (`nav/n_track_empty.png`). |
| 42 | P3 Smaller details | **Mostly fixed** | Done: `°C` units; humidity uses whole dots; marquee has soft edges on both sides (`r2_marquee/z_mq.png`); Find offline uses the phone-absent state; `STRAP 26°`; page dots hide while recording (`memo/r_rec_swipe_blocked.png`). Not done: `memo_leave()` still stops the recording (new #12). |

## New issues (most severe first)

1. **[P1] Call end prints the `[ CALL ENDED ]` label in black across the red disc.** When a
   call ends (End tap or remote hang-up), the label is drawn on top of the shrinking End disc.
   Its ink starts at black, so for one frame the disc reads `LL END` in black over the X. For
   the next 3 frames the disc and the grey label z-fight (`[ CALL●ENDED ]`). This happens on
   every call.
   Evidence:
   - `r2_end/z_en.png` (frame 2 after the End tap = `r2_end/en_002.png`)
   - `r2_missed/z_mi0.png` (`mi_000.png`, first frame after `phone sim callend`)
   - `call/k_ended.png` f2084-f2087

   Fix: draw the disc after the label (disc on top). Start the label's colour ramp only once
   the disc radius is below the gap between `CALL` and `ENDED` (≈ 8 px). Never ramp ink from
   `C_BG` over a non-black surface.

2. **[P1] Navigate: a background track has no indicator outside the app.** The track service
   keeps the GPS on and records after you leave, which is correct. But nothing on the face,
   launcher or control centre says so, and `svc_track_active()` is only used in `out_nav.c`
   and `svc_track.c`. A user who swipes away mid-walk has no way to know the GPS is still
   running, short of reopening Navigate. That drains the battery invisibly.
   Evidence: `r2_nav/n_face_rec.png`, `n_launcher_rec.png`, `n_cc_rec.png` (all taken 30 s into
   a recording).

   Fix: a system REC chip while `svc_track_active()` is true:
   - a red dot + `REC 12:34` in the face status slot (the face's only red);
   - a red dot on the Navigate launcher tile;
   - a `REC` row in the control centre that opens Navigate.

   Owner: shell + Navigate. Recorder needs the same chip (#12).

3. **[P2] Call: remote hang-up while ringing goes wrong in three ways.**
   - It is a hard cut. In one frame the title and hint vanish, Accept disappears, and
     Decline teleports from x 151 to x 233.
   - An `END` caption shows for a call that was never answered.
   - The final state says `[ CALL ENDED ]` when it should say missed.

   Evidence: `r2_missed/z_mi.png` (frame 0 ringing, frame 1 the cut).
   Fix: keep both discs in place and build them out together (dot dissolve, 200 ms). Show
   `[ MISSED CALL ]` under the name, with the title `CALL`, then dismiss as today.

4. **[P2] Red breaks §8 ("a dot or a single glyph — never an area fill") on four screens.**
   - Barometer storm: `[ STORM WARNING ]` is red text. It also replaces the trend line, so
     the -3.6 hPa / 3 h value that triggered the warning is hidden (`storm/s_storm.png`).
     Fix: a `C_RED` dot + `[ STORM ]` in `C_PRI` on the trend line, and
     `FALLING −3.6 hPa / 3 h` kept below it.
   - Noise over 85 dB: a red meter dot and a red `[ HEARING RISK ]` label, so two red
     elements, one of them text (`memo/r_noise_loud.png`). Fix: keep red only on the meter
     dots past 85 (overflow continues in red, §2.5). Draw the state label in `C_PRI`.
   - Call: Decline and End are 88 px solid red discs (`call/k_incoming.png`,
     `k_active_61s.png`). Fix: a `C_SURF2` disc with a red X glyph, or write an explicit §8
     exemption for call controls. Torch red light needs the same exemption, since it is the
     product function.
   - Recorder idle and recording use a red record dot / stop square inside a dark ring. That
     is within the rule (a single glyph). Keep it as the reference.

5. **[P2] Units don't keep case (§8 Labels).** One quantity is written two ways across the
   apps:
   - Barometer picker: `ALTITUDE 855 m` (`r2_baro/k_picker.png`).
   - Navigate: `ALT 919 M` (`r2_nav/n_reopen.png`).

   Non-compliant, all in caps:
   - Navigate: `KM/H` under the speed hero; `REC 00:36 45 M`; the `40 M` scale bar; `METRES`
     under the back-to-start hero (`nav/n_track_90s.png`, `nav/n_back_live.png`). That is
     three spellings of metres in one app.
   - Weather: `14 KM/H` (`wx/w_top_again.png`).
   - Cellular: `-81 DBM` (`cell/c_live.png`).
   - Memos: `7 S`, `3 S` (`memo/r_memos.png`, `r_playing.png`).
   - Barometer: `RISING +1.4 / 3H` (`baro/b_pressure.png`).

   Fix: `ui_label_unit` everywhere: `km/h`, `m`, `dBm`, `s`, `+1.4 hPa / 3 h`. Use the same
   unit-under-hero style as Noise's `dB`. Drop `METRES` for `m`.

6. **[P2] Navigate: Stop → `[ SAVED ]` → START TRACK are hard cuts.** At f1336 → f1337 the STOP
   pill and its full hold ring vanish, and `[ SAVED ]` + `45 M 00:37` appear in their place in
   one frame. The header flips from `● REC 00:37 45 M` to `3D FIX 9 SATS` in the same frame.
   The return to START TRACK is a cut too.
   Evidence: `r2_nav/n_hold.png` frames 18 → 19, `r2_nav/n_summary_after.png`.
   Fix: build the pill and ring out (dot dissolve 150 ms), then build the summary in (150 ms).
   Crossfade the header label with the colour-step ladder.

7. **[P2] Weather: new data replaces the whole screen in one frame.** When the phone's
   weather arrives, everything swaps at f88, about 3 s after open, with no morph:
   - city, LONDON → BENGALURU;
   - icon, partly cloudy → light rain;
   - hero, 17° → 18°;
   - condition, H/L and the facts row.

   This will happen on every update the phone pushes, not only on first connect.
   Evidence: `r2_wx_swap/z_sw.png` (`sw_087` → `sw_088`).
   Fix: `dot_text_morph` the temperature and H/L. Build the icon out and in (0.3 s). Crossfade
   the condition, city and facts with the colour-step ladder.

8. **[P2] Weather empty state is only an icon and `[ UPDATING ]`.** There is no head or body
   (§8 Empty), and there is a 150 px void between the icon (y 172) and the state (y 334). With
   no feed it says `UPDATING` forever, claiming activity that isn't happening.
   Evidence: `wx_cnone/icon_now.png`, `wxn/wxr_widget.png`.
   Fix: `ui_absent(cloud, "No weather yet", "Gadgetbridge sends it from your phone", NULL)` +
   `[ UPDATING ]` at y 340 for 30 s, then `[ NO DATA ]`. With the phone disconnected, use the
   §8 phone-absent state.

9. **[P2] Find watch: the text knockout slices ripple dots in half.** The fix for round-1 #14
   clips the ripple rings with rectangles around `FIND WATCH` and `TAP TO STOP`. The dots on
   the band edges come out as flat-cut half-moons above and below each line. A dot-matrix UI
   must never show a partial dot.
   Evidence: `find/z_fw_title.png`, `find/z_fw_hint.png` (from `find/fw_ringing.png`).
   Fix: skip whole dots whose centre is within (band half-height + dot radius) of the text
   line, as round-1 suggested, rather than clipping pixels.

10. **[P2] Music widget: press feedback is late, and returning shows a stale pressed state.**
    - Holding the widget's play disc for 6 frames shows no pressed state
      (`r2_music_wg/g_hold6.png`). The grey only appears after release
      (`music/m_wg_toggle.png` f841-f843).
    - Swiping back from Music to the pager reveals the widget's disc in the pressed grey,
      which steps back to white over 4 frames (`r2_music_wg/z_back.png`, `gb_003-gb_006`).
      This happens although the widget was only tapped to open.
    - Opening from the widget moves the disc, but the title and artist dissolve out, are
      absent for 3 frames (f305-f307), and then pop in at full white in the app position
      (`r2_music_wg/go.png`, `z_open.png`).

    Fix:
    - Show the pressed state after 2 frames of touch with < 8 px movement, as list rows do.
    - Clear the widget's pressed state when it opens the app.
    - Move the title/artist block with the disc (`EASE_STANDARD`, 300 ms) instead of
      dissolving it.

11. **[P2] Transitions draw dithered, soft-edged discs, against §1.4 (flat fills, no
    gradients or blur).** An ordered-dither halo appears in three places:
    - Weather widget tap: a dark stippled disc punches into the pressed card over `18°` and
      `Light rain` (`r2_wx_wg/z_wt3.png`, `wt_003`).
    - Music widget open: a stippled ghost ring around the disc, and stippled spheres where
      prev/next will be (`r2_music_wg/z_halo.png`, `go_003-go_004`).
    - Torch entry: the white disc grows with a wide dithered rim for 6 frames
      (`torch/t_entry.png` f3-f9).

    On the panel these read as texture noise. This is probably the engine iris; owner: engine.
    Fix: a hard-edged (1 px AA) circle for the iris, or a colour-step ladder of ≤ 8 flat
    levels. No ordered dither.

12. **[P2] Recorder: leaving still stops the recording, and now that's inconsistent with
    Navigate.** A side press during a recording saves the memo, and `[ SAVED ]` is visible for
    2 frames before the face slides in (`r2_recleave/rl.png` f164-f165). The new memo is
    `4 S` (`r2_recleave/rl_memos.png`). Navigate now keeps recording in the background and
    Recorder doesn't, so the same gesture has two outcomes. An accidental wrist press ends a
    lecture recording.
    Fix: a background recording service (like `svc_track`) behind the same system REC chip as
    #2. If that's out of scope this round, show the save on the face: a toast
    `SAVED MEMO 5 · 0:04`, so the stop is at least seen.

13. **[P3] Quick-tap pressed state lasts 2 frames, not the §8 minimum of 3.** Inbox row:
    `r2_quicktap/qi_000-001` #313031, `qi_002` #101010 (the push has started). Memo row:
    `qm_000-001` #313031, `qm_002` #181818. Hold the pressed fill for one frame after release.

14. **[P3] Recorder idle shows `00:00` in ghost cells.** §8 says never show `0` as a resting
    value, and keep ghost cells for faces and `--` (`memo/r_idle.png`). Show `--:--`, or the
    last memo's length in `C_DIS`.

15. **[P3] Navigate value changes.**
    - The fix arrival takes ~433 ms and passes a near-blank frame (`nav2/n2_fix.png` f129).
      Aim for 250 ms with no empty frame.
    - After the fix is lost, the dimmed speed still morphs 4.5 → 4.4 (`r2_nav/n_nofix_enter.png`
      f1553-f1559). Freeze stale values.
    - On refix the distance jumps 0 → 51 M (`r2_nav/n_refix.png`). Morph it.

16. **[P3] Compass Level.**
    - The long-press that enters Level gives no feedback while held
      (`compass2/c2_level_enter.png`).
    - The readout says `0.0°` while the bubble is still visibly off-centre (f78-f80), then
      hard-cuts to `[ FLAT ]` as the bubble snaps to centre (`compass2/c2_level_move.png`).

    Show a hold ring (as Nav's STOP does). Make the readout and the bubble agree, and
    crossfade to `[ FLAT ]`.

17. **[P3] Barometer polish.**
    - The ± glyphs are dots at ~3.4 px pitch and read as lines (`r2_baro/z_pm.png`); use the
      11×11 dot-icon grid.
    - Cancelling the picker flips the SEA LEVEL value 1013.3 → 1013.2 (`r2_baro/k_back_cancel.png`
      f436-f440).
    - `TEMPERATURE C` reads as a typo (`baro/b_local.png`). Use `TEMPERATURE` with `°C` on
      the hero.

18. **[P3] Buttons outside the §8 vocabulary.**
    - The Compass details CALIBRATE button is 150 wide at y ≈ 335, not 200 at y 372
      (`compass/c_details.png`).
    - Call, Cellular, Recorder, Find and Music all use 88 px icon discs with captions below
      (y 326-360), a pattern §8 doesn't define.

    Either convert them or add an "icon button" row to §8 (88 disc, caption `ui_label` 22 px
    below, pair at x 151/315) so they are standard rather than five variants.

19. **[P3] Find phone stop is a hard label swap.** `[ RINGING ]` vanishes, and the hint jumps
    from y ≈ 388 to y 356 in one frame (`find/f_stop.png` f222 → f223). Keep the hint fixed
    and build the state label out.

20. **[P3] Music Next: the old title hard-cuts out.** `music_next/mn_002` is a fully blank title
    band between the old and new titles. Ramp the old title down over 3 frames, then ramp the
    new one up.

21. **[P3] Weather icon details.**
    - Clear night's crescent is a vertical column with a heavy bottom row, which reads as a
      hook or an `L` (`z_wx_icons.png`, 9th).
    - Windy is three thin dotted lines and is much lighter than the other eight icons.
    - The hourly section still visibly builds while scrolling (round-1 #20).

22. **[P3] Call details.**
    - The comet passes 11 px from the captions and the name corner on every lap
      (`r2_comet/cm_031`, `cm_038`). Run it at r 222, or inset the name line to the chord
      minus 24 px.
    - `ON PHONE` as the active-call title is ambiguous; use `CALL` + `[ ON PHONE ]` under the
      timer.

23. **[P3] Torch red-mode gauge contrast.** Black text and dots on #7B0000 is 1.8:1
    (`torch/t_red_level.png`). Use `#2A0000` unlit, `#000000` lit, on a slightly brighter
    #9A0000 field while the gauge shows.

24. **[P3] Copy and spacing.**
    - `Start one on the NOW / page` widows (`nav/n_track_empty.png`).
    - Cellular's `Tap RADIO to turn on` is a sentence-case hint; §8 wants caps hints:
      `TAP RADIO TO TURN ON` (`cell/c_off.png`). This was round-1's own suggestion, which §8
      has since superseded.
    - Cellular live: `LTE / HOME` sits 10 px above the RADIO disc (`cell/c_live.png`). Give it
      16-20 px.

25. **[P3] Engine residue seen in these apps.**
    - The `NO SPEAKER MODULE` toast fades in over the `MEMOS` title, and the title shows
      through it for 3 frames (`r2_recleave/nt.png` f351-f353).
    - Anti-aliased text edges keep a +3/+4 G cast (#202420, #292C29), about 0.7% of pixels
      (`cell/c_inbox.png`).

## Already excellent (don't break these)

- **Performance.**
  - 0 late frames in all 45 runs.
  - Worst frame 24.4 ms (bus-bound full repaints on entry).
  - `--damagecheck` PASS on 21 scripts / 14,040 frames, including the new call morphs, the
    weather data swap, widget taps and the SMS pop.
- **The track service.** A track survives leaving the app, and distance stays continuous
  across a leave. Hold-to-stop has a dot ring that fills around the pill, and a plain tap
  teaches `HOLD TO STOP` (`nav/n_stop.png`, `r2_nav/n_hold.png`).
- **Lost fix.** `[ NO FIX ] 0:11 AGO` over dimmed last values, with the sky plot only after
  30 s. This is how a field tool should behave.
- **Baro picker.** Pushed screen; back = cancel. USE GPS is disabled in place, so Set never
  moves. Hold-repeat with acceleration and per-step morph. The pair sits exactly on the §8
  geometry.
- **Absent states.** One `ui_absent` layout with `<NAME> MODULE` footers. The phone-absent copy
  is identical in Music and Find.
- **Weather.**
  - Icons on one grid, and loops that rest after two plays.
  - The widget is the app's first screen, so tap → app is a true continuation with a pressed
    card (`wx/w_wg_tap.png`).
- **Inbox.** Fixed-width cards, 90 px pitch, a visible pressed fill, and it is revealed by back
  without rebuilding.
- **Call.**
  - Incoming → active is a real morph: Accept collapses, Decline glides to centre as End,
    the timer builds (`call/k_accept.png`).
  - Long names wrap at the hyphen and keep one position in every state.
- **Memos.** Elapsed time in the playing row. The no-speaker case is honest: dimmed play
  icons plus a toast.
- **Red discipline where it's right.**
  - The REC dot turns white on the track page, where the red "you" dot takes over.
  - The baro "now" column has one red dot.
  - Compass north is the dial's only red.
