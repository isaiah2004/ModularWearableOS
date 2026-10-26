# NOS critique: system (Settings, Modules, About) + cross-OS consistency, round 1

Reviewer build: `build/nos_sim_crit_system.exe`. Every output is under `build/nos/critic/system/`:
scripts in `scripts/` (s1 list, s2/s3/s6 sub-pages, s4 modules, s5 whole-OS tour, s7 notifications
empty, s8 About scroll, s9 pressed states, s10 no-phone), stills and contact sheets in `s1/`..`s10/`,
2x2 comparison grids in `grids/`, and the measuring scripts in `tools/` (`measure.py` counts red
connected components and finds the title band, `bands.py` lists the vertical content bands,
`blob.py` flood-fills a control to get its exact bbox). Pixel numbers below come from those tools,
not from eyeballing. Damage check: **PASS** on s1 (653 frames), s2 (864), s3 (1168) and s4
(5487). Cost model: **0 late frames** in every script. The worst frame is 24.4 ms, and that is only
the full-screen transitions.

## Verdict

The settings stack is well built. It is fast, the damage tracking is right, the pickers never
overshoot, the toggles and sliders animate cleanly, and the Modules page is the best diagnostics
screen I have seen on a hobby watch. Its "not connected" page is the best absent state in the OS.
What keeps it from being excellent is the detail work. The picker's hero number never moves under
the finger, and a tap on a neighbour value takes about 7 frames to land. Values and toggles pop in
and out at the bezel, and rows drift sideways while you scroll. A quick tap shows no pressed state.
Two pages contradict each other (Settings says `9/9` modules, Modules says `16/16`). Two numbers
are wrong: LTE shows "0 dBm" while searching, and caps turn mV into "MV" (megavolts). About clips
its first info row on the bezel. Across the OS the problem is consistency, not quality. Every
screen is decent, but they don't share a system: about 7 visual heights for action buttons (52 to
88 px), 10 screens off the title standard, three different wordings and two sizes for "module not
connected", four ways to say "no phone", page dots at five different heights, one red-count violation shown
(Barometer) plus two code paths that can put two reds on a screen, and entry builds from 0 to over
560 ms. Fix the standards
below once, in `ui.c`, and the OS will read as one product.

---

## Issues (most severe first). [A] = Settings/Modules/About, [B] = cross-OS

1. **[P1][A] Settings and Modules disagree on the module count.** The Settings > System >
   Modules row says `9/9` (only the 9 strap modules: `modules_present()` loops `i < 9`). The page
   it opens says `16/16 ONLINE` (board + strap, `M_COUNT`). With `present 0` they read `0/9` and
   `7/16`. Evidence: `s1/set_l6.png` (row value at x≈345, y 334), `s4/mod_l0.png`,
   `s4/mod_absent_l0.png`. Fix: have both call one counter (export `mods_count()` from
   set_modules.c) and show the same `N/16`. If you prefer strap-only, change the Modules hero to
   `9/9` with "STRAP" as its label and drop the board dots from the summary row.

2. **[P1][A] Cellular detail reports "0 dBm" while searching.** After POWER ON, the hero is `0`,
   with the unit `DBM 0/4 BARS` and the line `SEARCHING`. 0 dBm is an impossibly strong signal.
   Evidence: `s4/d_lte_on.png`. Also `"%s %s"` with an empty operator leaves a trailing space, so
   "SEARCHING" sits 7 px left of centre (x 164–289, centre 226). Fix: while
   `registered != 1 && != 2`, hero `--`, unit `SEARCHING`, and no dot bar. Build line 2 without the
   empty `%s`.

3. **[P1][A] About clips its board row on the bezel at rest.** At first view
   "ESP32-S3-Touch-AMOLED-" is drawn at y≈400 across x 66–400, but the glass there spans only
   x 71–395. The "E" and the trailing "-" are cut. Evidence: `s3/about_from_set.png`,
   `s5/t_about.png`. Fix: break the name as `ESP32-S3-Touch-` / `AMOLED-1.43-B` (each line is
   about 200 px), or drop the logo block 8 px (ATOP 70→62). The break is the better fix.

4. **[P1][A] Picker taps feel late.** A tap on the next value (`10` under the hero) calls
   `pk_step()`, which only starts the 0.5 s spring. `idx`, the morph, the commit and the haptic
   `FB_TICK` all wait until the spring passes the half-step. In the frames, the tap releases at
   f320 and the hero starts morphing at f325–f326, about 200 ms after the finger touched.
   Evidence: `s2/pick_tapnext.png`. The same code serves Set time (`pk_step` in `time_input`). Fix:
   in `pk_step` set `p->idx = t`, then call `pick_commit()`/`pick_texts(0)` and
   `nos_feedback(FB_TICK)` at once. Let the spring move only the rulers (skip the tick when
   `pk_tick` later sees the same idx).

5. **[P1][A] The picker hero doesn't follow the finger.** A 100 px drag moves only the
   4–7 px ruler dots at x=70/396. The hero morphs in place (15→30→60, scrambling between).
   Evidence: `s2/pick_drag.png` f249–f258. The neighbour values (C_DIS, pitch 3) don't move
   either. This breaks rule §1 ("everything the finger touches follows it 1:1"). Fix: draw the hero
   at `hy - (p.x - idx*PK_STEP)` and the neighbours with the same offset, clipped to y 110–350, so
   the drum visibly turns. Keep the morph only for the settle.

6. **[P1][B] The "module not connected" state has three wordings, two sizes and four layouts.**
   Measured headline cap height and width: "Not connected" in UI_F_TITLE (cap 22 px) on Compass,
   Barometer, Navigate and Modules; "Module not connected" in the smaller face (18 px) on Heart
   rate, Cellular and Recorder. The body runs 1 line (Cellular, Recorder), 2 lines (Compass, Baro,
   Nav, Modules) or 3 lines (Heart). The footer is `QMC5883L STRAP` / `BME280 STRAP` /
   `M10 GPS STRAP` (C_DIS at y≈398), `NO PHONE NEEDED` (Cellular, y 345), `EXPECTED ON` + wiring
   (Modules, y 342/367), or nothing (Heart, Recorder). The headline sits at y 250–275 in apps and
   y 222–243 in Modules. Inside Settings there are three more forms: `NEEDS THE LIGHT MODULE`
   (sentence-like label), `[ NO SPEAKER ]`/`[ NO MOTOR ]` (ui_state), and the toast
   `No light sensor`. Evidence: `grids/g14.png`, `grids/g15.png`, `grids/g16.png`,
   `s4/d_haptic_absent.png`, `s3/bright_absent.png`, `s3/vol_absent.png`,
   `s3/bright_absent_tap_toast.png`. Standard: see table B9.

7. **[P1][B] The Barometer's red element is 7 dots.** The history chart's "now" column is a
   full red column (red dot components at x 345, y 274–334). It is the only screen with more than
   one red element. Evidence: `s5/t_baro.png`, `measure.py` output "red comps 7". Fix: draw the
   last column white like the rest and put a single red dot (d 8) on top of it, i.e. the "now"
   dot.

8. **[P1][B] The heart widget hides a missing module.** With `present 0` it still shows `68 BPM`,
   `JUST NOW` and `TAP TO MEASURE` (`s5/a_widget_2.png`), and the tap opens an app that then says
   "Module not connected". Fix: with no HR module, use hero `--` (C_DIS, ghost cells) and the label
   `NO HR MODULE` in place of `JUST NOW`, and drop "TAP TO MEASURE". Same rule for any widget whose
   data needs a module.

9. **[P1][B] "No phone" is said four different ways.** Music: dim BT icon, `[ NOT CONNECTED ]`
   (white ui_state), and body "Connect your phone". Find phone: icon tile, TITLE
   "Phone not connected", and `OPEN GADGETBRIDGE`. Settings > Bluetooth: `[ WAITING FOR PHONE ]`
   and "Pair in Gadgetbridge as NOS". The Settings row value says `NO PHONE`. Evidence:
   `s10/np_music.png`, `s10/np_find.png`, `s6/m_bt_nophone.png`, `s6/m_conn_nophone.png`. Standard:
   table B9.

10. **[P2][A] Values and toggles pop in and out near the bezel.** `draw_entry` drops the value
    when `rx - 20 - label_w > lend` fails, and the toggle when `rx - 76 > lend - 8` fails, so they
    vanish in a single frame as a row crosses that y. Fling evidence: `s1/set_fling_down.png` f557
    (Cellular, no value) → f558 (`OFF` appears); `s1/set_l1.png` (Screen timeout at y 53 without
    its `15 S`); `s3/set_absent_l3.png` (Cellular without `No module`). Fix: never hide them. Fade
    the value with the row's `k`, and if it can't fit, shift the label's left edge instead (rows
    near the bezel are already inset).

11. **[P2][A] Rows drift sideways during a vertical scroll.** `lx = max(row_span(cy,30), 62)`, so
    a row's left edge slides from x 62 to x 85–131 as it nears the top or bottom. Resting
    `s1/set_l0.png`: Brightness x 63, Screen timeout x 85. While scrolling, every label moves
    diagonally. Modules does the same (`s4/mod_l1.png`: Touch x 163 at the top, Battery x 123 at
    the bottom). In a launcher-style focus list that is a signature, but in a settings list it
    reads as a wobble. Fix: keep `lx = 62` and `rx = 404` while `|cy − 233| < 150`. Beyond that,
    inset only the rows that would actually be clipped, and let them fade (qfade already does).

12. **[P2][A] Quick taps show no pressed state.** The highlight waits `press_t > 0.05 s`, and a
    67 ms tap is over before it shows, so the row pushes its page with no visual acknowledgement.
    In `s2/bright_entry.png` f29–f31 nothing lights up before the push. The highlight does exist on
    a hold (`s9/p_set_row_pressed.png`). The same applies to Modules rows and the haptic/speaker
    play rows. Fix: on IN_TAP of a still list, draw the pressed pill for 3 frames (and keep it on
    the outgoing screen during the push).

13. **[P2][A] The Settings entry build is hidden under the iris.** Rows rise 14 px with a 34 ms
    stagger over 0.40 s, starting at the same instant as the 0.42 s iris. In
    `s5/e_settings.png` the rows are done by f2172 while the iris runs to f2180, so the stagger is
    invisible and only costs full repaints. Modules (`s5/e_modules.png`) is mostly visible because
    its hero runs to 0.32 s. Fix: start `build_t0` at `nos_t + 0.17` when entered by TX_IRIS
    (iris 40 %), or skip the build when the iris is the entry. Push entries (from Settings) don't
    need the delay.

14. **[P2][A] The Wi-Fi tile flashes brighter on release.** Measured centre colour: pressed
    #9C999C (f317, f318) → released #D5D2D5 (f319) → #949194 → … → #181818. The fill brightens
    for a frame before fading off. Evidence: `s3/wifi_tap.png`. Cause: `fill = ui_mix(pressed ?
    C_SURF3 : C_SURF2, pressed ? C_SEC : C_WHITE, on)` switches palette the moment `pressed` clears.
    Fix: keep a `press_k` spring (SNAPPY) and mix the pressed colours by it, so release eases from
    grey instead of jumping to white.

15. **[P2][A] The Bluetooth tile looks like a switch but does nothing.** It is the same 152 px
    white/SURF2 disc as Wi-Fi's working toggle, but taps are ignored: no colour change, no haptic
    (`s3/bt_tap.png` f248–f257 identical). Fix: draw Bluetooth as a bare icon over the state text
    (as the Modules Bluetooth detail does, `s4/d_ble.png`). If it stays a tile, a tap should give
    `FB_TAP` plus a toast ("Bluetooth stays on for the phone").

16. **[P2][A] The Brightness "Auto" row's absent label crowds the bezel and misnames the part.**
    `NEEDS THE LIGHT MODULE` is left-aligned at x 104 and runs to x 410, 11 px from the glass at
    y 370. A tap toasts "No light sensor", and the Modules row calls the part "Light". Evidence:
    `s3/bright_absent.png`, `s3/bright_absent_tap_toast.png`. The toast also lands on the row at
    y≈375 and covers it. Fix: use `[ NO LIGHT MODULE ]` (ui_state, centred, y 368, like Volume's
    `[ NO SPEAKER ]`) and the toast "No light module".

17. **[P2][A] The Volume and Haptic-strength sliders look live with no hardware.** With `present 0`
    the fill stays white and draggable, and only `[ NO SPEAKER ]` in #666 hints otherwise
    (`s3/vol_absent.png`). Fix: draw the fill in C_DIS and the icon in C_SURF2 when the module is
    absent (the value still saves).

18. **[P2][A] Flip rows have no affordance and value right edges don't line up.** Wrist and Units
    look like plain text. Their values right-align at x 404 while chevron rows end at x 384: in
    `s1/set_l6.png` LEFT ends at 388, METRIC at 404, 9/9 at 383. Fix: give flip rows the same
    `rx − 20` value edge and a small two-way dot glyph (or the chevron), so every interactive row
    ends at x 404 with an affordance.

19. **[P2][A] The same value is formatted differently on different pages.** Step goal: list
    `8,000`, picker hero `8000`, neighbours `7500/8500` (`s2/pick_goal.png`). SD free space:
    Settings row `29 GB FREE`, Modules row `29 GB FREE`, Storage hero `29.0`, Modules > SD card
    hero `29.0`, footer message `Simulator SD` in body (Storage) vs `SIMULATOR SD` caps (Modules).
    Fix: one `fmt_steps`/`fmt_gb` in ui.c. Dot-matrix 5x7 has a comma glyph, so use `8,000`. Use
    `29` for ≥ 10 GB everywhere.

20. **[P2][A] Units are corrupted by forced caps.** `ui_label` upper-cases everything:
    `4002 MV` (millivolts read as megavolts, Modules row, Battery detail, About), `39 DB` / `DB SPL`
    / `PEAK 39 DB`, `914.6 HPA`, `FIELD 48 UT` (µT). Evidence: `s4/mod_l1.png`, `s4/d_batt.png`,
    `s4/d_mic.png`, `s4/d_env.png`, `s8/ab_2.png`. Fix: show the battery as `4.00 V`. For dB/hPa/µT,
    add `ui_label_raw()` (same font and tracking, no case change) for unit tokens, or rephrase the
    labels without the unit (`SOUND LEVEL`, unit in the hero).

21. **[P2][A] The Storage page duplicates Modules > SD card with different geometry.** Hero y 120
    vs 128, bar y 262 h 28 vs y 260 h 26, a `% USED / MICROSD` row vs none, the message in body vs
    caps label (`s3/storage_0.png` vs `s4/d_sd.png`). Fix: have Settings > Storage push the Modules
    SD detail (`open_detail(M_SD)`), or share one draw function.

22. **[P2][A] Partial progress segments break the "discrete" rule.** `ui_segments` draws the
    fractional segment as a narrower bar: Storage 9 % → 1.8 segments, so segment 2 is shown about
    80 % wide (x 105–116 vs 93–103), and Battery at 78 % draws a narrow 8th segment
    (`s3/storage_0.png`, `s4/d_batt.png`, HR quality bar in `s4/d_hr_2.png`). Fix: round `value` to
    a whole segment in `ui_segments`, or add a `discrete` flag defaulting to 1.

23. **[P2][A] The Time zone picker uses three dot sizes plus a word in dot matrix.** It has the
    hero TALL pitch 8, the neighbours 5x7 pitch 3, and `13:24 LOCAL` in 5x7 pitch 2.6, with "LOCAL"
    set as dot-matrix text. That makes four type sizes with the label (`s2/pick_tz_drag.png`). The
    Step goal picker repeats it (`6.0 KM`). Fix: draw the extra line as `ui_label` C_SEC (`13:24
    LOCAL`, `6.0 KM`) at y 400.

24. **[P2][A] The Motion detail's axis line nearly touches the glass.**
    `X +0.05  Y +0.04  Z -1.00 G` spans x 45–422 at y≈350, where the glass is x 36–430: margins
    of 9 and 8 px (`s4/d_imu.png`). Fix: split it into `X +0.05  Y +0.04` / `Z -1.00 G` at y 344
    and 370, and move the gyro line to 396.

25. **[P2][A] Modules rows open pages but show no chevron.** Settings rows that push a page show a
    dot `>`. Modules rows, which also push, show none (`s4/mod_l1.png`). Fix: use the Settings
    chevron at `rx`, with the value at `rx − 20`.

26. **[P2][A] The Modules detail hero size varies by page for no reason.** Pitch 6 (Motion, Clock),
    7 (summary), 8 (the rest). Motion's `3°` (2 glyphs) is pitch 6, half the visual weight of
    Light's `320`. Evidence: `s4/d_imu.png` vs `s4/d_light.png`. Fix: use pitch 8 for any hero of
    ≤ 5 glyphs. Only the 8-glyph clock earns pitch 6.

27. **[P2][A] The Touch test has no title and shows a stale coordinate.** It is the only Modules
    page without a title, and on entry it shows `LAST 233, 381`, the tap that opened the page
    (`s4/d_touch.png`). Fix: clear `V.touched` in the touch page's enter, and add `TOUCH` as a
    ui_label at y 120 (the top ring sits at y 52, so the title can't go at 70).

28. **[P2][A] The Reset icon reads as a loading spinner.** It is a 12-dot arc whose 3-dot
    arrowhead is the same 6 px size as the arc (`s6/m_reset_0.png`), which is exactly what the
    spec bans ("never spinners"). Fix: make the head 3 dots of d 9 forming a clear `<` at the arc
    start, or reuse the power-symbol construction with an arrow.

29. **[P2][B] Destructive actions are confirmed three different ways.** Settings > Restart pushes
    a confirm page with a white primary button. Power sheet > RESTART fires at once (secondary
    pill). Power sheet > POWER OFF arms to a red `TAP AGAIN`. Alarm DELETE is a danger outline.
    Evidence: `s3/restart_0.png`, `s5/t_power.png`. Standard in table B3.

30. **[P2][B] Title placement differs on 10 screens.** See table B1.

31. **[P2][B] Action buttons come in about 7 visual heights.** See table B3.

32. **[P2][B] Page dots sit at five different heights.** Centre y: home pager 440, Heart rate app
    375, Barometer 433, Navigate 437, Recorder 445. On faces they also collide with the bottom
    complication: the Dot face's `78%` (y 437–450) is overlapped by the dots at y 457 after a pager
    trip (`s5/t_home_back.png`, `s5/t_face_1.png`, `s5/t_face_4.png`). Standard: y 440 for every
    pager. Faces must keep y 425–455 clear, or the dots fade out 1 s after the pager settles.

33. **[P2][B] No face shows the unread indicator.** sys_home.c:318 says "the face draws its own
    (face_ctx.unread)", but no `nos/faces/*.c` reads `unread` (grep), so with 6 unread
    notifications the face shows nothing. Widget pages do show the red dot at (233, 24)
    (`s5/t_widget_1.png`). Fix: draw the same dot at (233, 24) from sys_home on page 0 too when the
    face reports no `unread` support. For the Dot face, the red colon could become the unread
    indicator and stop blinking.

34. **[P2][B] Two code paths can put two reds on one screen.** (a) The Activity widget over goal
    draws red overflow ticks (`ui_tick_ring`, ui.c:500) and the unread dot (sys_home.c:321) on the
    same page. (b) A banner without an icon draws a red dot (ui.c:650) over the Dot face's red
    colon. Couldn't be shown (the sim pedometer ignores `set steps`), but both paths are in the
    code. Fix: skip the unread dot while the page's own red is lit (expose a `has_red()` hook on
    nos_widget), and give icon-less banners a white dot.

35. **[P2][B] Hint text uses two styles.** Caps label in C_DIS: `TAP TO MEASURE`, `TAP TO START`,
    `TAP TO RECORD`, `TAP FOR RED LIGHT`, `TOUCH EVERY RING`. Sentence-case body: Find phone
    `Tap to ring your phone` (C_SEC, y 354–376) and Find watch `Tap to stop` (white, over the dot
    rings it collides with). Evidence: `grids/g08.png`, `grids/g10.png`. Standard: the caps label.

36. **[P2][B] Entry builds run from 0 ms to over 560 ms in different vocabularies.** See table
    B10. The worst cases are Activity (hero shows `0` for 5 frames, then counts up past frame 17),
    Navigate (no build at all), Music (controls scale up from a point, outside the dot vocabulary)
    and Torch (3 black frames).

37. **[P3][A] Copy widows.** "Alarms and timers keep / running." (`s3/restart_0.png`),
    "Pair in Gadgetbridge as / NOS" (`s6/m_bt_nophone.png`), "A felt click on every / detent"
    (`s4/d_haptic_absent.png`). Fix: wrap width 300→340 on confirm pages (the glass is 458 wide at
    y 276), and rewrite "Pair it in Gadgetbridge" and "A click on every detent".

38. **[P3][A] The Reset settings button ignores the bezel dim.** Rows fade (qfade) near the edge,
    but the red outline is drawn at full strength at y 383 (`s1/set_l7.png`). Fix: pass `k` into
    the danger button colours.

39. **[P3][A] The About rows' build ends at 0.57 s.** Delay `0.10 + r*0.034` for r = 8, plus
    0.2 s. The text rows also appear before the logo has built (`s5/e_about.png` f2276–f2280:
    tagline and version first, logo dots after), which reverses the hierarchy. Fix: rows start at
    `0.20 + min(r,5)*0.034`, so the logo leads and everything is done by 0.37 s.

40. **[P3][A] About mixes three value styles.** Grotesk (`1.0.0 (Oct 11 2026)`, board, display),
    dot-matrix pitch 3.2 (`6.0 MB`, `7 KB`, uptime, rail) and caps label (`SIL OPEN FONT LICENSE
    1.1`) (`s8/ab_1.png`, `s8/ab_2.png`). Fix: use dot matrix only for live numbers (uptime, rail)
    and grotesk for static values. Memory is static enough to be grotesk.

41. **[P3][A] Slider tap snaps don't match the slider's detents.** A tap snaps to 5 %
    (`floorf(v/5+0.5)*5`), but the track's detent dots and the haptics are every 10 %. Fix: snap
    taps to 10 %.

42. **[P3][A] Small polish items.** The Watch face row could show the current face (`DOT`). The
    Cellular row pushes the whole Cellular app (with SMS) where Bluetooth and Wi-Fi get a radio
    page. The Modules list ends with ~200 px of black under Heart rate (`bottom_pad 183`,
    `s4/mod_l5.png`); 120 is enough. 4 frames mid-fling repaint the full screen (f559–f562
    24.4 ms in `s1/frames.csv`, not late).

43. **[P3][B] Missing-value placeholders differ.** `--` (Modules, Music `--:--`) vs `-` (Weather
    HUMID/WIND, `s10/np_weather.png`). Standard: `--`.

44. **[P3][B] The two brightness sliders differ.** Settings: 320×72 track, sun icon pitch 2.6
    inside the fill. Control centre: 235×50, a larger rayed sun (`s5/t_control.png` vs
    `s2/bright_0.png`). Keep both sizes, but share the icon pitch rule (`h*0.065+1`) and the 10 %
    detent dots.

---

## B. Cross-OS consistency audit

Method: `s5_tour.txt` opens every launcher app (registry.c order), the four overlay screens
(alarm_ring, timer_ring, call, find_watch), the launcher, the face picker, the power sheet, the
control centre, notifications (list, detail, banner, empty), toast, the 5 widget pages and the 8
faces. It records the entry (`e_*.png`) and the settled still (`t_*.png`), then repeats everything
with `present 0` (`a_*.png`). Comparison grids: `grids/g01..g16.png`, entry sheets
`grids/ent_00..12.png`. All y values are panel pixels.

### B1. Screen title

House standard (`ui_title`): ALL-CAPS label font, cap height 12 px at **y 58–69**, colour
**#9C9C9C (C_SEC)**, tracking 2, centred, fading and rising with scroll.

| Screen | Title | y band | colour | verdict |
|---|---|---|---|---|
| Settings + all 8 sub-pages | SETTINGS / BRIGHTNESS / … | 58–69 | C_SEC | standard |
| Modules + details (not Touch) | MODULES / MOTION / … | 58–69 | C_SEC | standard |
| Heart, Breathe, Timer, Stopwatch, Alarms, World clock, Barometer, Navigate, Cellular, Recorder, Find phone, Call, Notifications | name | 58–69 | C_SEC | standard |
| Weather | city name `BENGALURU` | 58–69 | C_SEC | standard (city as title is fine) |
| Find watch | FIND WATCH | 58–69 | C_SEC | position ok, but drawn over the dot rings (a stray dot at y 46) |
| About | none (dot logo y 102–186) | – | – | **deviates**; acceptable if the logo counts as the title |
| Touch test | none | – | – | **deviates** (#27) |
| Activity | none (icon + ring) | – | – | **deviates** |
| Music | none | – | – | **deviates** |
| Compass | none (dial numerals at y 52–79) | – | – | exempt: full-bezel dial |
| Torch | none | – | – | exempt: full-screen light |
| Launcher | APPS | **139–150** | C_SEC | **deviates**: 80 px low |
| Control centre | `10:09 • 78%` | 44–55 | white | sheet header, exempt |
| Notification detail | icon + `CALENDAR  NOW` | 46–67 | white/C_SEC | detail header, acceptable |
| Alarm ring | ALARM | **86–97** | C_SEC | **deviates** |
| Timer ring | TIME'S UP | **118–131** | **white** | **deviates** in both y and colour |
| Power sheet | none (battery hero y 74–127) | – | – | sheet, acceptable |
| Widget: Music | MUSIC | 58–69 | C_SEC | standard |
| Widget: Weather | city | **84–95** | C_SEC | **deviates** |
| Widget: Timer | TIMER | **126–137** | C_SEC | **deviates** |
| Widget: Heart / Activity | none (icon at the top) | – | – | **deviates** |

**Standard:** every pushed screen and every widget gets `ui_title()` at y 58–69 in C_SEC. Exempt
only screens whose dial owns the top band (faces, Compass, Torch, Activity's ring, Find watch's
rings) and sheets. Changes: Launcher APPS → 58–69. Alarm ring and Timer ring → `ui_title` (keep
`TIME'S UP` but in C_SEC; the hero carries the alert). Widget Weather and Timer → 58–69. Widget
Heart → `HEART RATE`. Music app → `MUSIC` (the widget already has it). Touch test → see #27.

### B2. Header / back affordance

| Screen | back affordance |
|---|---|
| All pushed screens | none drawn. Swipe right from the left edge (engine). Verified on Settings and every sub-page, the Modules Touch test (SCR_HDRAG, `s4/d_touch_back.png`) and the Brightness slider (SCR_HDRAG, `s2/bright_back.png`). Side button goes home. |
| Sheets (control, notifications) | reverse swipe at the scroll limit, button |
| Overlay screens (alarm/timer ring, call, find watch) | their own buttons |

This is consistent: no screen draws a back chevron, so no fix is needed. P3 suggestion: the first
time each pushed screen opens, do a single 8 px "peek" (the content nudges right and settles) to
teach the gesture, since nothing on screen hints at it.

### B3. Action buttons (exact bboxes from `blob.py`)

| Screen | label | kind | w × h | centre y | hit area |
|---|---|---|---|---|---|
| Alarms (empty) | ADD ALARM | primary white | 200 × **88** | 334 | – |
| Stopwatch | START / LAP | primary / disabled | 140 × **88** (pair, cx 310/154) | 350 | – |
| Timer | CUSTOM | secondary | 196 × **88** | 374 | – |
| Alarm edit | DELETE | danger outline | 196 × **88** | – | – |
| Alarm ring | SNOOZE | secondary | 170 × **88** | 262 | – |
| Timer ring | STOP / +1 MIN | primary / secondary | 124 × **88** (pair) | 300 | – |
| Power sheet | SLEEP / RESTART / POWER OFF | secondary / danger | 288 × **72**, 236 × 72 | 222 / 306 / 390 | – |
| Settings confirm | RESTART / RESET | primary / **red fill** | 200 × **68** | 372 | 240 × 90 |
| Notification detail | OPEN ON PHONE / DISMISS | secondary / ghost | 260 × **68** | 300 / 384 | – |
| Settings list | RESET SETTINGS | danger outline | 260 × **68** (drawn 256×64) | in list | 120 (4 units) |
| Set time | SET | primary | 156 × **64** | 386 | 200 × **80** |
| Modules details | RESYNC / POWER ON / CALIBRATE / DONE | secondary / primary | 200 × **64** | 388 | 240 × **80** |
| Notifications | CLEAR ALL | secondary | 200 × **64** | end of list | – |
| Barometer | Calibrate / Use GPS / Set | secondary / primary | 196 × **60**, 150 × 60 | 360 | – |
| Navigate | Start track / Stop | primary / secondary | 176 × **60** | 374 | – |
| Compass | Calibrate | secondary | 150 × **52** | 335 | – |
| Breathe | 1 / 3 / 5 MIN | chips | 96 × **56** | 370 | – |

Inconsistencies: about 7 visual heights (52, 56, 60, 64, 68, 72, 88) and widths from 124 to 288
for a single primary action. Several hit areas are under the 88 px rule (Set time and Modules: 80;
Compass: 52 visual). Restart is confirmed by a full page in Settings but is immediate in the Power
sheet. The Settings Reset page uses a red **fill**, while Power off arms to a red fill only after a
first tap.

**Standard (the Time group already does it):** visual height **88**, radius 44. A single action is
200 wide, centred at **y 372** (bottom 416, inside the glass at x 133–333). A pair is 140 × 88 at
cx 154 / 310. Primary = white/black ink. Secondary = C_SURF2. Lists: destructive = BTN_DANGER
outline. Final confirm = BTN_RED fill. One confirmation pattern for destructive or disruptive
actions: the **arm-then-tap-again** pattern of the Power sheet (no extra page, no surprise),
applied to Settings Restart and Reset, Alarm DELETE and Power sheet Restart. If the Settings confirm
pages stay, the Power sheet's Restart must arm too. Chips (Breathe) may stay 56 tall, but need
88 px hit rows.

### B4. List rows and cards

| Screen | row pitch | text x range | pressed state | push affordance | value position |
|---|---|---|---|---|---|
| Settings | 90 (3×30), headers 60 | 62–404, inset to the circle per row (drifts) | SURF2 pill, after 50 ms | dot chevron at 396–404 | label C_SEC, right edge 384 |
| Modules | 90, headers 60, summary 180 | 62–404 + dot at lx+5, drifts | SURF2 pill | **none** | label, right edge 404 |
| Modules haptic/speaker play list | **88** | **70–396** | SURF2 pill | play disc r 22 | – |
| About | 26-px units, rows 78–104 | centred | none (static) | – | centred under label |
| Launcher | 92, curved focus list | icon at x 61→137 along the bezel | – | – | – |
| Notifications | cards, ~150 + 28 gap | text 87–375 | card press | tap card | right-aligned time label |
| World clock | cards, 96 pitch | **card width varies** 382/408/347 px | – | – | dot time right |
| Recorder list | cards, ~100 pitch | **card width varies**, text truncated (`10...`) | – | play disc | – |

**Standard:** Settings rows (90 px, section headers 60 px, C_DIS label), with three fixes: no
sideways drift (#11), a chevron on every row that pushes (Modules too, #25), and one value right
edge at x 384 (#18). Play lists move to 90 px and 62–404. For cards, use the notification card
(fixed width, r 40, SURF2): World clock and Recorder cards stop varying width per row and use the
notification card's width, and the Recorder's `10...` truncation goes away at the fixed width. The
Launcher's curved focus list is its signature and is exempt.

### B5. Label typography and tracking

| Use | font | tracking | case | where it deviates |
|---|---|---|---|---|
| `ui_label` (house) | UI_F_LABEL | **2** | upper | – |
| Music elapsed/remaining | UI_F_LABEL | 1 | as given | con_music.c:340–345 |
| Navigate readouts | UI_F_LABEL | 1 | upper | out_nav.c:447–449 |
| Barometer axis `-24H` `NOW` | UI_F_SMALL | 1 | upper | out_baro.c:275 |
| Compass degree numerals | UI_F_SMALL | 0 | – | out_compass.c:270 |
| Segment face `STP KCAL MIN` | UI_F_SMALL | 0 | upper | face_segment.c:212 |
| Modules bus line `I2C 0x38  FT3168` | UI_F_SMALL | 0 | **mixed** | set_modules.c:370 |
| Modules absent wiring `Motor driver on GPIO16` | UI_F_SMALL | 0 | **mixed** | set_modules.c:655 |
| `ui_clock_small` | UI_F_SMALL | 1 | – | ui.c:397 |
| Cell SMS badge | UI_F_LABEL | 0 | – | con_cell.c:230 (ok, tight badge) |

**Standard:** all caps labels through `ui_label` (tracking 2). UI_F_SMALL is reserved for
technical identifiers and also gets tracking 1. Mixed-case wiring text becomes caps. Units need
the case-preserving variant (#20).

### B6. Red usage per screen (count of red elements; measured with `measure.py`)

| Screen | red elements | what | verdict |
|---|---|---|---|
| Face Dot | 1 (blinks 0/1) | colon | ok |
| Face Glyph / Inverted | 1 | second dot | ok |
| Face Life | 1 | now dot | ok |
| Face Tape | 1 | dot before the date | ok |
| Face Matrix / Split / Segment | 0 (Split: HR out of range; Segment: second lap) | – | ok |
| Widget pages (all 5) | 1 | unread dot (233, 24) | ok, but see #33/#34 |
| Activity widget over goal | **2 possible** | overflow ticks + unread | **violation path** (#34) |
| Banner without icon over the Dot face | **2 possible** | banner dot + colon | **violation path** (#34) |
| Stopwatch | 1 | second dot at 12 o'clock | ok |
| Compass app | 1 | north marker | ok |
| Barometer | **7** | "now" column | **violation** (#7) |
| Recorder | 1 | record button 52 px | ok |
| Power sheet | 1 | POWER OFF outline + text | ok |
| Settings list | 1 | Reset settings (when visible) | ok |
| Settings Reset confirm | 1 | RESET fill | ok |
| Modules Compass detail | 1 | north dot | ok |
| Modules Heart rate detail | 1 | trace head | ok |
| Torch (red mode) | full-screen red | night-vision light | functional exception, document it |
| Control, Notifications, Launcher, Faces picker (except the face's own), Activity, Heart, Breathe, Weather, Music, Timer, Alarms, World clock, Navigate, Cellular, Find phone, About, Settings sub-pages, Modules list and other details, Call, Find watch | 0 | – | ok |
| Alarm ring, Timer ring | 0 | – | P3: an alert is a listed red use. One red dot (e.g. the ring's lead dot) would mark them as alerts |

### B7. Dot-matrix numerals (pitch from source; height = measured band)

| Screen | value | font / pitch | height px | ghost cells |
|---|---|---|---|---|
| Face Dot | HH/MM | TALL 18 | ~170 each | yes |
| About logo | NOS | bitmap 12 | 84 | – |
| Recorder playback | time | TALL 11 | – | – |
| Heart app / widget | BPM | TALL 10 | 88 | widget when empty |
| Settings Timeout picker | 15 | TALL **9** | 72–81 | no |
| Breathe, Navigate, Weather app, Alarm ring, Timer | – | TALL 9 | 79–82 | no |
| Settings TZ / Step / Set time / Storage / sliders | – | TALL **8** | 70–72 | no |
| Modules details (most) | – | TALL 8 | 72 | no |
| Activity app + widget | steps | TALL 8 | 86 incl. ghost | **yes** |
| Barometer, Compass, Weather widget, Stopwatch | – | TALL 8 | 72–75 | no |
| Modules summary, Call, Baro calibrate | – | TALL 7 | 61 | no |
| Modules Motion / Clock, Power sheet, Recorder idle | – | TALL **6** | 52–54 | Recorder: ghost-only idle |
| Activity sub-stats, Heart sub-stats, Stopwatch tenths | – | TALL 5 | – | – |
| Timer tiles, Alarm list | – | TALL 4.4 | 40 | no |
| World clock rows | – | TALL 4.0 | 41 | no |
| Picker neighbours, About live values, Timer presets | – | 5x7 3.0–3.2 | ~22 | no |
| TZ/Step extra line | – | 5x7 2.6 | ~18 | no (#23) |

Spread: hero pitches 6, 7, 8, 9, 10, 11 for the same role (the one big number on a screen). Ghost
cells appear only on the faces, Activity and the empty heart widget. Dot size 0.8 × pitch is
consistent everywhere (good).

**Standard, three steps:** **H1 pitch 9** for a hero of ≤ 4 glyphs (Timeout, Breathe, Weather,
Navigate already do it). **H1-wide pitch 8** for 5–8 glyphs (times, `+8:45`, `914.6`). **List
pitch 4.4** for rows and tiles (Timer and Alarms already). Secondary numbers are 5x7 pitch 3.
Nothing else is allowed. Heart's pitch 10 drops to 9. Modules Motion and Power drop pitch 6 and use
the step their glyph count gives (#26). Ghost cells only on faces and for the empty value `--`;
Activity drops them.

### B8. Empty states (no data, not a missing module)

| Screen | icon | headline | body | extra |
|---|---|---|---|---|
| Notifications | white dot bell in a dot ring, y 104–252 | `No notifications` **BODY** C_PRI, y 287 | – | `[ ALL CAUGHT UP ]` ui_state |
| Alarms | alarm icon C_SEC, y 149–202 | `No alarms` **BODY** C_PRI, y 248–265 | – | ADD ALARM primary 200×88 |
| Settings Storage, no card | SD icon C_DIS pitch 4 | `No card` **TITLE** | 2-line body C_SEC | – (not reachable in sim: `hal_storage_get` is always mounted) |
| Modules SD, no card | SD icon C_DIS pitch 4.2, y 156 | `No card` **TITLE**, y 244 | body | message SMALL |
| Heart widget, no reading | – | ghost `--` | `TAP TO MEASURE` | – |
| Weather, no phone | – | data with `-` placeholders | – | – |

**Standard:** icon pitch 4.2 in C_DIS centred at y 156 (Modules), headline UI_F_TITLE in C_PRI
at baseline 244, at most 2 body lines in C_SEC from 282, and then **either** one `[ STATE ]` at
y 340 **or** one primary 200×88 CTA at y 372. Notifications and Alarms switch their headline to
TITLE. Notifications keeps its dot ring, which is a good signature.

### B9. Module-absent and phone-absent states

| Screen | icon | headline (size) | body | footer |
|---|---|---|---|---|
| Modules detail (9 strap pages) | C_DIS 4.2, y 133–178 | `Not connected` TITLE, y 222–243 | 2 lines, y 264–311 | `EXPECTED ON` + wiring, y 342 / 367 |
| Compass | C_DIS, y 144–207 | `Not connected` TITLE, y 254–275 | 2 lines | `QMC5883L  STRAP` C_DIS, y 392 |
| Barometer | C_DIS, y 150–189 | `Not connected` TITLE | 2 lines | `BME280  STRAP` |
| Navigate | C_DIS, y 162–207 | `Not connected` TITLE | 2 lines | `M10 GPS  STRAP` |
| Heart rate | C_DIS, y 148–191 | `Module not connected` **BODY**, y 250–267 | **3 lines** | none |
| Cellular | C_DIS, y 138–206 | `Module not connected` **BODY**, y 270–287 | 1 line | `NO PHONE NEEDED` |
| Recorder | C_DIS, y 138–213 | `Module not connected` **BODY** | 1 line | none |
| Settings Brightness | – | – | – | `NEEDS THE LIGHT MODULE` label + toast `No light sensor` |
| Settings Volume / Haptics | – | – | – | `[ NO SPEAKER ]` / `[ NO MOTOR ]` |
| Settings Cellular row | – | – | – | value `No module` (hidden near the bezel, #10) |
| Modules list | grey status dot | – | – | value `ABSENT` |
| Heart widget | **none: still shows 68 BPM** | – | – | – (#8) |
| Music, no phone | dim BT icon | `[ NOT CONNECTED ]` ui_state, white | `Connect your phone` | – |
| Find phone, no phone | icon in tile | `Phone not connected` TITLE | – | `OPEN GADGETBRIDGE` |
| Settings Bluetooth, no phone | BT tile | `[ WAITING FOR PHONE ]` | `Pair in Gadgetbridge as NOS` | – |

**Standard (Compass, Baro, Nav and Modules already agree on most of it):** title stays, icon C_DIS
pitch 4.2 centred at y 170, `Not connected` in UI_F_TITLE at baseline 270, one or two body lines
saying what the module adds (C_SEC), and a footer `PART  STRAP` ui_label in C_DIS at y 398.
Modules keeps its extra `EXPECTED ON` wiring line because it is the bring-up tool. Heart, Cellular
and Recorder move to it (headline size, 2-line body, footer `MAX30102  STRAP`,
`EG800AK  STRAP`, `INMP441  STRAP`). For Settings' inline notes, `[ NO <MODULE> MODULE ]` ui_state
everywhere, with the toast using the same words. Phone absent: the same layout with the BT icon,
`Phone not connected`, body "Open Gadgetbridge on your phone", footer `GADGETBRIDGE`. Music, Find
and Settings > Bluetooth all adopt it, and the Settings row value becomes `NOT CONNECTED`.

### B10. Entry animation (frame 0 = before `open`, iris starts at f1; from the `e_*.png` sheets)

| Screen | style | content done at | notes |
|---|---|---|---|
| All launcher apps | engine iris 0.42 s, EASE_STANDARD | iris done ~f13 | consistent |
| Pushed pages (Settings subs, Modules details) | slide (SPRING_PAGE) + 0.30 s random dot hero | ~f9 (300 ms) | visually lands by f6, springs to f15 |
| Settings | rows rise 14 px + fade, 34 ms stagger | f5, hidden under the iris | #13 |
| Modules | hero random 0.32 s + 16 status dots 12 ms apart | f10 (330 ms) | good |
| About | logo dot sparkle 0.40 s, rows +0.10 s stagger | rows f2, logo f10 | hierarchy reversed (#39) |
| Activity | ring sweep + count-up from `0` | **> f17 (> 560 ms)** | shows `0` for 5 frames: reads as wrong data |
| Heart rate | heart icon reveal, trace instant | f5 (170 ms) | – |
| Breathe | START disc **scales up**, radial dots, chips fade grey→white | f9 (300 ms) | scaling is outside the dot vocabulary |
| Weather | icon dots + hero random | f9 (300 ms) | good |
| Music | controls **scale up from a point**, ring sweep | f8 (270 ms) | no dot build |
| Timer | tiles fade by row + digits random | f8 (270 ms) | good |
| Stopwatch | digits random + buttons fade from grey | f9 (300 ms) | – |
| Alarms (empty) | icon centre reveal, text and button fade through greys | f8 (270 ms) | white button through 4 greys reads as translucency |
| World clock | cards fade + digits random | f5 (170 ms) | – |
| Compass | ticks sweep + hero random | f9 (300 ms) | good |
| Barometer | hero random + chart columns rise | f8 (270 ms) | good |
| Navigate | **none** (rings appear with the iris) | f2 | missing build |
| Cellular | bars rise per column, tiles, badge **pops** at f11 | f11 (370 ms) | badge pops with no transition |
| Recorder | ghost digits + record disc scales + waveform | f9–f12 | – |
| Find phone | icon strokes in | f7 (230 ms) | good |
| Torch | **black f2–f4**, then dither ramp to white | f10 (330 ms) | 100 ms of black looks like a hitch |
| Alarm ring / Timer ring | digits random + buttons fade | f9 (300 ms) | good |
| Call | text fade only | f8 | no dot build |
| Find watch | rings expand from the centre | continuous | good |
| Launcher | rows arrive 2 frames apart | f5 | good |
| Faces picker | ring + name pill fade | f12 (400 ms) | – |

**Standard:** the Settings sub-page build, i.e. the hero builds with `DOT_REVEAL_RANDOM` over
**0.30 s**, icons with `DOT_REVEAL_CENTER`, rows stagger 34 ms (≤ 6 rows), and buttons appear with
the hero at full colour (no grey fade-ups). For iris entries, start the build at 40 % of the iris
(+0.17 s) so it is seen. No scale-ups (Music, Breathe, Recorder), no count-up longer than the
build (Activity: count from 0 over 0.30 s and never show `0` as a resting value), and every screen
builds something (Navigate: build the rings dot by dot; Call: build the caller name's initials or
the ring). Torch: start the dither on f2.

### B11. Page dots

| Screen | centre y | pitch | notes |
|---|---|---|---|
| Home pager (faces + widgets) | 440 | 14 | collides with face bottom complications (#32) |
| Heart rate app | **375** | 14 | – |
| Barometer | **433** | 14 | – |
| Navigate | **437** | 14 | – |
| Recorder | **445** | 14 | – |

**Standard:** `ui_page_dots(…, y = 440)` everywhere.

### B12. Placeholders, units and number formats

| Thing | variants seen | standard |
|---|---|---|
| Missing value | `--` (Modules, Music `--:--`), `-` (Weather), `0` (LTE dBm, #2) | `--` |
| Thousands | `8,000` (Settings), `8000` (picker), `2840` (Activity), `2841` (faces) | comma at ≥ 10,000 only, so 4-digit counts stay compact; the picker matches the list |
| Units in caps labels | `MV`, `DB`, `HPA`, `UT` | case-preserving units (#20) |
| Storage | `29 GB FREE`, `29.0` | `29` (≥ 10 GB), `9.4` (< 10 GB) |

---

## What is already excellent (don't break it)

- **Performance and correctness.** Damage check passes on all 8,172 frames of the four system
  scripts. Zero late frames. Settled Settings, picker, slider and confirm screens cost 1.5 ms,
  scrolling the list 5–15 ms (p95 12.9 ms in `s1`).
- **Picker physics.** `pk_go` caps the hand-off velocity, so flicks and rubber-band releases land
  without overshoot (`s2/pick_flick.png`, `s2/pick_rubber.png`), with `FB_LIMIT` at the ends and a
  tick per detent.
- **Slider tap-to-position.** A monotonic 13-frame spring from 100 % to 5 % with a morphing hero
  (`s2/bright_tap.png`). Brightness never goes below 5 %.
- **Toggles and flip rows.** A snappy thumb travel in 5 frames, haptic felt before sound or
  haptics turn off, and the Wrist/Units value cross-fade with an 8 px slide (`s2/wrist_flip.png`).
- **Revealed-pop behaviour.** Settings, Modules and About keep their scroll and skip the build when
  a sub-page closes.
- **Modules "not connected" page** (`s4/d_haptic_absent.png`): icon, headline, what the module
  adds, and exactly where it should be wired. This is the model for every absent state in the OS.
- **The Modules summary.** `16/16` plus one dot per part with a gap between board and strap gives
  the whole bus at a glance, and it updates at 4 Hz without flicker.
- **Reset confirm.** One red element, a darker red when pressed (`s6/m_reset_pressed.png`), plus
  success haptic and toast.
- **The house title** (`ui_title`) is identical on about 25 screens. The standards above extend
  what already works.
- **The back gesture** works even on SCR_HDRAG pages (Touch test, sliders).
