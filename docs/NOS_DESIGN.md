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
