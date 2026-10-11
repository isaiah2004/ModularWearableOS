# NOS — design and engineering spec

NOS is the Nothing-inspired OS for the Waveshare ESP32-S3-Touch-AMOLED-1.43-B
(466x466 round AMOLED, ~326 ppi, touch, one side button) plus the strap modules
in `docs/strap_layout.html`. Branch `nothing-os`. Read this before writing any screen.

References: `docs/research/NOTHING_DESIGN_REFERENCE.md` (visual language, with
sources), `docs/research/ZSWATCH_NOTES.md` (OS ideas, Gadgetbridge protocol).

## 1. Experience goals

- **Flawless 30 fps.** Every frame is exactly 1/30 s of motion. Nothing stutters,
  nothing waits. If a screen can't make 30 fps on the device cost model, it is
  redesigned, not tolerated.
- **Apple-level feel, Nothing-level look.** Everything the finger touches follows
  it 1:1 and keeps its velocity on release. Everything is interruptible.
- **Unmistakably Nothing.** True black, white ink, dot-matrix numerals, ALL-CAPS
  mono labels, discrete dot/segment progress, one red dot at most.
- **Every module is real.** Each strap module has a home in the UI, a live data
  view, and a graceful "not connected" state.

## 2. Visual rules (non-negotiable)

1. Canvas `C_BG` true black. Content in `C_PRI`/`C_WHITE`; hierarchy by greys
   (`C_SEC` #999, `C_DIS` #666, `C_LINE2` #333, `C_LINE` #222, surfaces `C_SURF`
   #111 / `C_SURF2` #1A1A1A).
2. **Red `C_RED` (#D71921) appears at most once per screen** and only as: the
   "now"/live indicator dot, recording, alerts/destructive, over-limit, the
   second-hand dot. Many screens have no red at all.
3. Typography per screen: at most 3 sizes, 2 families. Hero numbers in dot
   matrix (`dot_text*`, `DOT_FONT_TALL`, dot = 0.8 x pitch). Words in the grotesk
   (`UI_F_TITLE`, `UI_F_BODY`). Micro labels `ui_label()` = ALL-CAPS mono with
   tracking (`BPM`, `STEPS`, `MON 12`). States as bracketed text: `[ SAVED ]`,
   `[ SEARCHING ]` (`ui_state()`), never spinners.
4. Surfaces: circles, pills, big-radius cards (radius 24-40). Flat fills only —
   no shadows, gradients, blur or translucency. On = white fill + black ink;
   off = `C_SURF2` + white ink.
5. Progress is discrete: `ui_segments`, `dot_bar`, `dot_ring`, `ui_tick_ring`.
   Overflow past a goal continues in red.
6. Icons are dot-matrix (`dot_icon`, 11x11 grid). No coloured app icons.
7. Round screen: content lives in the circle. Use `ui_row_span(y, margin)` to
   inset rows. Titles sit at y≈70 (`ui_title`), primary content centred, actions
   in the lower third (y 330-400) where the thumb is. Nothing important in the
   corners of the bounding square.
8. Touch targets ≥ 88 px. Spacing scale 8/16/24/32/48.

## 3. Motion rules

Nothing's motion is mechanical and precise: **click, not swoosh**. No bounce on
anything the user operates.

- Finger-driven: critically damped springs (`mspring`, `SPRING_PAGE`/`SHEET`/
  `SNAPPY`), launched with the release velocity (`msp_kick`). Scrolling: `mscroll`
  (iOS decay 0.998/ms, rubber band c=0.55, edge bounce by spring).
- Non-interactive: `mtween` with `EASE_STANDARD` (arrivals), `EASE_ACCEL`
  (departures), `EASE_MECH` (precise snaps). Micro 150-250 ms, transitions
  300-420 ms.
- Signature: **dot build-ins** (`dot_text_anim`, `dot_icon_anim`) when a screen
  appears — hero numbers assemble from dots over ~10 frames; **digit morphs**
  (`dot_text_morph`) when a value changes; dot bursts on tap feedback where it
  helps; ring dots fill one by one.
- Stagger lists by ≤ 2 frames per row, ≤ 8 rows.
- Haptic tick (`nos_feedback(FB_TICK)`) on every detent: list row snaps, pager
  page changes, picker values, slider steps of 10%. `FB_TAP` on taps,
  `FB_TOGGLE_ON/OFF`, `FB_SUCCESS`, `FB_ERROR`, `FB_LIMIT` at hard limits.
- Animation state advances only in `tick(dt)`. `draw()` is pure: it is called once
  to record and once per 16-row stripe (~30x per frame) and must not mutate state
  or call slow HAL functions. Read sensors/services in `tick` into statics.
- Quantise slowly varying visual parameters (fades, alphas) to ≤ 16 levels so
  damage settles and still screens cost zero.

## 4. Navigation map

```
                      [Control centre]  (swipe DOWN from face, TX_DROP, follows finger)
                             ^
 [Face picker] <-long press- [ FACE ] -swipe LEFT-> [widgets: Activity | Heart | Weather | Music | Timer]
                             |
                             v
                      [Notifications]   (swipe UP from face, TX_RISE, follows finger)

 Side button: on face -> Launcher (TX_IRIS from the button side); anywhere else -> home.
 Button double press -> last app.  Button long press -> power sheet.
 Swipe RIGHT anywhere (pushed screens) -> back, interactive (engine-owned).
 Launcher row tap -> app opens with TX_IRIS from the tapped icon (set nos_iris_x/y).
 Tapping a widget or a face complication deep-links into its app.
```

Sheets (control centre, notifications) close with the reverse swipe once their
content is at its scroll limit (engine-owned), or the button.

## 5. Performance budget (device cost model)

Frame = 33.3 ms. Bus: RGB565 full screen = 23.4 ms (fits). CPU raster (core 0)
must stay under ~25 ms for a full-screen frame, ~12 ms for typical frames.
Measured costs (ns per covered pixel, docs/FLAT60_DESIGN.md): fill 4, AA circle 25,
pill 32, sprite blit 33, mask/glyph 130-150, blend 190, text 280, ring 300, arc 507,
line 723. Per primitive call per stripe ~0.4 us.

Rules:
- Big static art (dials, tick rings, illustrations) is drawn once into a sprite
  (`core/sprite.h`, PSRAM) and blitted, or relies on damage tracking (it doesn't
  move, so it costs nothing after the first frame).
- Anything that moves every frame must be cheap: fills, circles, pills, dots,
  sprites. Avoid `gfx_line`/`gfx_arc` in per-frame motion over large areas.
- Keep primitive counts per screen < 250 (GFX_MAX_CMDS 320; overflow = full repaint).
- No big static arrays in internal RAM (> 256 B): the device's internal DRAM is
  nearly full. Use `hal_big_alloc` (PSRAM) at first use.
- Check every screen with the sim cost model (`docs/NOS_SIM.md`): p95 frame
  estimate < 30 ms in motion, 0 late frames when idle.

## 6. Code conventions

- C99, `-Wall -Wextra` clean. Files: `nos/apps/<group>_<name>.c`,
  `nos/faces/face_<name>.c`. One screen = one `const nos_screen scr_<name>`.
- Include `nos.h`, `ui.h`, `dot.h`, `motion.h`, `svc.h`, `hal_ext.h`, `prefs.h`.
- State in file-static structs, reset in `enter()` where appropriate.
- Input: handle `IN_TAP`, `IN_DRAG_*` (axis-locked), `IN_LONG`, `IN_BTN*`.
  Return 1 when consumed. Don't implement back yourself — the engine does it.
  Screens with horizontal content (pagers, sliders) set `SCR_HDRAG`.
- Pressed states: on `IN_DOWN` record the pressed control, show it pressed (fill
  `C_SURF3` / grey), act on `IN_TAP`, clear on `IN_UP`.
- Empty/absent states: every screen that depends on a module checks
  `hal_ext_present()` and shows a designed state (dot icon + "Module not
  connected" + one line on what it adds), not a blank.
- Timers/alarms/stopwatch run in services (`svc.h`), so they survive leaving the app.
- Strings in English, sentence case for body, ALL CAPS only via `ui_label`.

## 7. Polish checklist (every screen, every loop)

- [ ] Layout centred and balanced inside the circle; nothing clipped by the bezel.
- [ ] One focal element; hierarchy obvious at a glance (arm's length, 1 s).
- [ ] Typography ≤ 3 sizes; labels are mono caps with tracking.
- [ ] Red used ≤ 1 time and only for a meaning listed in §2.
- [ ] Entry animation: something builds in (dots, stagger) — subtle, ≤ 400 ms.
- [ ] Every touch has immediate visual + haptic feedback; pressed states exist.
- [ ] Motion follows the finger, carries velocity, no overshoot.
- [ ] Values change with a morph/tick, never a hard flicker.
- [ ] Empty, loading, error and module-absent states designed.
- [ ] Device cost model: no late frames in the screen's motion scripts.
- [ ] Damage check passes (no stale pixels).
- [ ] AOD variant where relevant (faces): < 10% lit pixels, no red fills.

## 8. Standards (adopted after the round-1 consistency audit, docs/polish/system_r1.md §B)

Constants live in `nos/ui.h` (`UI_*`). Use them; never local numbers.

| Property | Standard |
|---|---|
| Title | `ui_title()` baseline y 70, C_SEC, every pushed screen and widget. Exempt: faces, Compass dial, Torch, full-screen rings (Activity ring, Find watch), sheets. |
| Back | engine swipe only; no chevrons. |
| Buttons | height 88, radius 44 (`UI_BTN_H`). Single: 200 wide centred at y 372. Pair: 140 wide at x 154 / 312, y 372. Primary white/black, secondary C_SURF2. Hit area ≥ 88 px everywhere. Chips may be 56 tall with 88 px hit rows. |
| Destructive / disruptive confirm | arm-then-tap-again: first tap arms (red outline + "TAP AGAIN TO …" + 3 red 6 px dots expiring one per second), second tap acts. Used by power off, restart, reset settings, delete alarm. No confirm pages. |
| List rows | 90 px pitch, headers 60. Content x 62–404 while \|cy−233\| < 150 (no sideways drift), values/chevrons right-aligned at x 384, a chevron on every row that pushes. Pressed pill shows ≥ 3 frames even on a quick tap. Cards: fixed width, r 40, C_SURF2. |
| Labels | `ui_label` (caps, tracking 2). Units keep case: `ui_label_unit(value, unit)` → "4.00 V", "39 dB", "914.6 hPa", "48 uT". UI_F_SMALL only for technical identifiers (caps, tracking 1). |
| Red | one element per screen, as a dot or a single glyph — never a column/area fill. Over-limit / alerts / recording / live. No amber. Banners without an icon use a white bell. |
| Dot numerals | hero ≤ 4 glyphs pitch 9; hero 5–8 glyphs pitch 8; rows/tiles 4.4; secondary 5x7 pitch 3. Ghost cells only on faces and on the empty value `--`. A value with a degree mark is centred as a group. |
| Empty | `ui_absent(icon, head, body, footer|NULL, p)`; then either one `[ STATE ]` at y 340 or one primary button at y 372. |
| Module absent | Page: `ui_absent(icon, "Not connected", what it adds, "<NAME> MODULE", p)`, the same body and footer in the app and in Modules (Modules adds the part and pins on a C_DIS wiring line at y 372). List rows: `NO MODULE`. Notes and toasts: `NO <NAME>` with the short name from `ui_module_short()` (HAPTICS, MIC, SPEAKER, GPS, LTE, CLIMATE, COMPASS, HR SENSOR; LIGHT SENSOR in notes only, too long for a toast). |
| Phone absent | `ui_absent(DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as Bangle.js NOS", NULL, p)`. |
| Entry build | hero `DOT_REVEAL_RANDOM` 0.30 s, icons `DOT_REVEAL_CENTER`, rows stagger 34 ms (≤ 6 rows), buttons with the hero at full colour (no grey fade-ups), iris entries start their build at 40 % of the iris, no scale-ups, no count-up longer than the build, never show `0` as a resting value. Screens revealed by back (`nos_revealed()`) or pulled by a finger (`nos_entered_by_finger()`) don't rebuild. |
| State changes | never a hard cut: dot build-out/in, colour-step crossfade (≤ 8 levels) or a rigid slide, 150–300 ms. Values change with `dot_text_morph` (~0.28 s), at most ~1 Hz for live readings. |
| State words | always `ui_state()` brackets, in the owning app's words: `[ SEARCHING ]` (GPS / modem), `[ MEASURING ]`, `[ NO CONTACT ]`, `[ WAITING ]`, `[ OFF ]`. Counts go on a plain label under the state. |
| Icon-disc actions | For actions an icon names better than a word (Cellular RADIO / SMS, Find, Call): 88 px discs (r 44), centres 128 px apart (x = 169 / 297), disc centre y 360, caption `ui_label` 16 px under the disc in C_SEC. Off: C_SURF2 disc, C_PRI icon; on: white disc, black icon; pressed: C_SEC disc, black icon, held ≥ 3 frames. A badge (count) sits at the disc's 1:30 position, r 14, white. |
| Page dots | `ui_page_dots` at y 440 (`UI_DOTS_Y`). |
| Placeholders | missing value `--` (`UI_NONE`). Thousands separator only ≥ 10,000. |
| Hints | caps labels (`ui_label`), not sentence-case body text. |
| Toasts | top of screen (y≈46), bracketed by `ui_draw_toast` itself — pass plain words. |
