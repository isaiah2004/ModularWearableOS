# NOS simulator

`build/nos_sim.exe` runs the real NOS engine and apps (everything under `nos/`) on the PC,
through the same 16-row stripe path the watch uses. It has three jobs:

1. an **interactive window** for trying the UI by hand,
2. **headless scripted runs** on a virtual clock (screenshots, contact sheets, GIFs) for
   automated review,
3. a **device cost model** that estimates what every frame would cost on the ESP32-S3,
   plus a **damage check** that catches stale pixels.

## Build

```powershell
.\sim\nos_build.ps1            # -> build\nos_sim.exe (incremental, parallel)
.\sim\nos_build.ps1 -Run       # build and open the window
.\sim\nos_build.ps1 -Clean     # rebuild everything
.\sim\nos_build.ps1 -Out nos_sim2.exe    # when nos_sim.exe is open and locked
.\sim\nos_build.ps1 -Verify    # also prove the cost hooks don't change pixels
```

Sources are discovered automatically: every `.c` under `nos/` (apps, faces, fonts, ...),
the core modules NOS uses (`gfx font sprite anim notif battery pedometer walk motion_wake
wake_service`), `core/fonts/*.c` and `sim/nos_*.c`. Add a file and it is built; no edits.

- Objects live in `build/nos_obj/`. A source recompiles when it, or any header under
  `core/ nos/ sim/`, is newer than its object.
- Linking goes to a temp file that is then swapped in. If `nos_sim.exe` is running (and so
  locked), it is renamed to `nos_sim.exe.<time>.old` first; those are deleted by later
  builds. Teams can also keep their own exe with `-Out nos_sim_<team>.exe`; all builds share
  the object cache safely.
- Works in PowerShell 7 (parallel compile) and Windows PowerShell 5.1 (sequential).
- **A file that fails to compile links its last good object** (yellow WARNING) so one team's
  half-edited file does not block the others. `-Strict` makes any error fatal. A file that has
  never compiled is always fatal.
- `-DNOS_FONT_SHIM` is added automatically until `nos/fonts/nos_fonts.h` exists.
- No weak symbols: MinGW/PE weak definitions neither act as cross-object fallbacks nor
  reliably merge with a strong definition, so every symbol has exactly one definition.

## Interactive window

```
build\nos_sim.exe [--scale 1.5] [--selftest] [--fullframe]
```

The 466 px panel at 1:1 (or `--scale`), round mask, dark bezel. The engine runs at exactly
30 fps (high-resolution waitable timer, absolute deadlines); the window only repaints on
frames that sent pixels, and input is handled while waiting.

| Input | Action |
|---|---|
| mouse drag / click | touch (sampled at mouse rate, like the touch controller) |
| Space or B | side button (hold for long press, tap twice for double) |
| G | next air gesture: left, right, up, down, near, far |
| S | screenshot -> `build/nos_out/shot_HHMMSS_mmm.png` |
| R | start/stop recording -> `build/nos_out/rec_NNN.gif` |
| F | frame-cost HUD (panel on the right, outside the circle) |
| D | outline this frame's dirty rects (green; red = full repaint) |
| P / N | pause / single-step one frame |
| Esc | quit |

The HUD shows the screen on top, the estimated ESP32 frame time (red when late), raster
and bus time, dirty pixels, rects, damage commands and primitive calls, the top primitive
classes, p95/max/late over the last 120 frames, and a graph with the 33.3 ms budget line.

Settings persist in `build/nos_out/store/` (interactive only).

## Scripted runs

```
build\nos_sim.exe --script FILE [--out DIR] [--boot] [--selftest]
build\nos_sim.exe --shots-all                 # tools/nos_tour.txt -> build/nos_shots/
python tools\nos_shots.py run FILE            # run + DIR/index.html
```

The clock is **virtual**: every frame is exactly 33.333 ms, nothing depends on wall time,
so the same script always produces the same frames (the RTC boots at 2026-10-11 10:09:30,
the store starts empty, sprite memory is zeroed). A 30 s script runs in well under a second.
The boot animation is skipped (popped before frame 1) unless `--boot` is given.

`DIR` defaults to `build/nos_out/<script name>/` and receives the images, `frames.csv`,
`summary.txt` and `log.txt`. The exit code is 1 if any `expect` failed or a command was
invalid.

### Commands

One per line; `#` starts a comment. Coordinates are panel pixels (0-465), centre 233,233.

| Command | Effect |
|---|---|
| `wait MS` (or `wait 2s`) | run frames for that long |
| `frames N` | run N frames |
| `tap X Y` | press 67 ms, release |
| `hold X Y [MS]` | long press (default 700 ms) |
| `drag X0 Y0 X1 Y1 [MS]` | finger moves with ease-in-out (stops before release, no fling); default 400 ms |
| `flick X0 Y0 X1 Y1 [MS]` / `swipe` | ease-in, released at full speed (a fling); default 120 ms |
| `touch X Y 0/1` | raw touch sample, no frames |
| `press` / `longpress [MS]` / `doublepress` | side button |
| `button 0/1` | raw button state |
| `air left/right/up/down/near/far` | APDS-9960 gesture |
| `open NAME [X Y]` | `nos_open()` the screen with the iris (from X,Y, default centre) |
| `open! NAME` | push it instantly (TX_NONE) |
| `root NAME` | replace the whole stack with that screen |
| `home` / `pop` | `nos_home()` / `nos_pop()` |
| `skipboot` | pop the boot screen if it is on top |
| `face N` | `nos_prefs.face = N` |
| `notify APP\|TITLE\|BODY` | `svc_notif_add_local()` + `nos_banner()` |
| `companion APP TITLE...` | a notification through the HAL queue (`hal_notification_poll`) |
| `phone LINE` | `svc_phone_inject(LINE)` (companion protocol line) |
| `banner TITLE \| BODY` / `toast TEXT` | engine overlays directly |
| `present MASK` | `hal_ext_sim_present` (e.g. `present 0` = no modules, `0x1FF` = all) |
| `time HH:MM[:SS]` / `date YYYY-MM-DD` | set the simulated RTC |
| `set KEY VALUE` | see below |
| `shot NAME` | `NAME.png` of the current frame |
| `film NAME N [COLS] [DIV]` | the next N frames as one contact sheet `NAME.png` (thumbs at 1/DIV, default 2), each labelled with index, frame number and estimated ms (red = late) |
| `film! NAME N ...` | same, plus every frame as `NAME_000.png ...` |
| `gif NAME N` | the next N frames as an animated GIF, exact 30 fps timing |
| `mark NAME` | start a section: frames.csv and the summary group by it |
| `log TEXT` | print with the frame number (also `log.txt`) |
| `expect top NAME` | fail unless the top screen's `name` is NAME |
| `expect depth N` | fail unless the stack depth is N |
| `expect idle` | fail if the last frame still sent pixels |
| `expect present MASK` | fail unless `hal_ext_present()` == MASK |
| `expect nolate` | fail if any frame in the current section is estimated late |
| `include FILE` | run another script inline |
| `quit` | stop |

`film` and `gif` **start a capture and return immediately**; the frames come from the
commands that follow. If the script ends first, the sim keeps running frames until every
capture is complete:

```
gif open_heart 30      # record the next second...
open heart             # ...of the iris opening
wait 1000
```

`NAME` for `open` is the C name without `scr_`, case-insensitive: `boot home control notifs
notif_detail launcher faces power activity heart breathe timer stopwatch alarm worldclock
compass baro nav weather music cell find memo torch settings modules about alarm_ring
timer_ring call find_watch selftest`. (Table in `sim/nos_sim.c`, keep it in sync with
`nos/apps/nos_apps.h` and `nos/svc.h`.)

`set` keys: `time HH:MM:SS`, `date Y-M-D`, `tz MIN` (offset for `hal_epoch_seconds`,
default 330), `battery PCT`, `charging 0/1`, `steps N`, `present MASK`, `imu AX AY AZ` / `imu
off`, `aod 0/1`, `full 0/1` (`nos_debug_force_full`), `selftest_bug 0/1`. Any other key is
offered to `nos_ext_sim_set(key, value)` in `sim/nos_ext_sim.c` (module data such as heart
rate or GPS); unknown keys fail the script.

### Touch realism

Touch samples are delivered at 60 Hz (two per frame, at the frame's midpoint and end, with
the virtual clock set to those instants), so the engine's least-squares velocity sees the
same sample spacing as the real controller. `drag` uses ease-in-out (velocity ~0 at
release), `flick` ease-in (released at about twice the average speed: 200 px in 120 ms
leaves the finger at ~3300 px/s).

## Device cost model

The watch cannot be measured right now, so every frame is priced with the costs measured
on it (docs/FLAT60_DESIGN.md, `core/gfx_bench.c`):

| class | ns/px | where |
|---|---|---|
| fill | 4 | gfx_clear, gfx_fill_rect, ui_rrect solid spans, engine clears |
| circle | 25 | gfx_fill_circle |
| pill | 32 | gfx_fill_pill |
| blit | 33 | sprite_blit, sprite8_blit |
| mask | 131 | dot.c stamps |
| glyph | 146 | font_draw / ui_text with fonts >= 96 px line height |
| blend | 190 | gfx_px_blend, ui_rrect / ui_rrect_line AA pixels |
| text | 280 | font_draw / ui_text, smaller fonts |
| ring | 300 | gfx_ring |
| arc | 507 | gfx_arc (and gfx_sweep) |
| icon | 641 | vector icons |
| line | 723 | gfx_line, gfx_taper |
| dim | 8 | nos.c dim_rows (transition dimming) |
| select | 6 | nos.c composite_two (iris / dissolve layer select) |

- Pixels are the pixels each call actually **wrote** inside its clipped stripe and dirty
  rect (not bounding boxes). Checked against "pixels that changed" for the bench shapes:
  exact (`tools/gfx_cost_exact.c --accuracy`).
- Every raster-mode primitive call costs **0.4 us**, counted per call: the engine calls
  each screen's `draw()` once per 16-row stripe of every dirty rect, so a primitive drawn in
  a 466-row repaint costs 30 calls even where it clips away.
- `raster_ms = sum(px * ns) + calls * 0.4 us`; `bus_ms = dirty_px * 2 / 18.6 MB/s`
  (RGB565 over QSPI); the flush runs on the other core in parallel, so
  `est_frame_ms = max(raster_ms + 1.5, bus_ms) + 1.0`. A frame that sent nothing costs
  1.5 ms (record + diff). **late** = `est_frame_ms > 33.3`.
- The record pass (draw() with `fb->rec`) is covered by the fixed 1.5 ms, not priced per
  primitive.

Hooks: `core/gfx_cost.h` defines `GFX_COST(cls, px)` (one call + px pixels) and
`GFX_COST_PX(cls, px)` (pixels only). Without `GFX_COST_MODEL` (the firmware) both are
`(void)(px)`, so the firmware is unchanged. Instrumented: `core/gfx.c`, `core/font.c`,
`core/sprite.c`, `nos/ui.c` (ui_text, ui_rrect, ui_rrect_line), `nos/nos.c` (clear_sub,
dim_rows, composite_two). A new rasteriser that writes pixels itself should add one
`GFX_COST(class, pixels_written)` per call (see the header comment).

Pixel safety: `nos_build.ps1 -Verify` renders 400 random scenes of every instrumented
gfx/font/sprite primitive (random clips, translations, stripes) with the hooks and with the
`HEAD` sources and compares the bytes. For the whole sim: build with `-NoCostModel -Out
nos_sim_nocost.exe` and compare `--hashes` output of the same script.

### Output

`frames.csv`, one row per frame:

```
frame,dirty_px,rects,cmds,est_raster_ms,est_bus_ms,est_frame_ms,late,calls,full,section,
screen,pc_record_us,pc_raster_us,ms_fill,ms_circle,...,ms_select
```

`summary.txt` (also printed): p50/p95/max estimated ms over all frames and over frames that
sent pixels, late count and %, per `mark` section, raster time share by class, and the five
worst frames with their dominant class. `python tools/nos_shots.py late DIR` lists the most
expensive frames; `report DIR` writes an HTML page with the table, shots, sheets and GIFs;
`diff OLD NEW` shows changed shots (`NEW/diff_*.png`: old | new | magenta diff) and the
per-section p95/late deltas.

## Damage check

```
build\nos_sim.exe --damagecheck FILE [--out DIR] [--selftest]
```

Runs the script twice in child processes, once with damage tracking and once with
`nos_debug_force_full = 1` (every frame a full repaint), hashing the glass (inside the round
mask) every frame. Identical runs print `PASS`. Otherwise it reports the first differing
frame, re-runs both up to that frame and writes `DIR/damagecheck_fN.png` (damage-tracked |
full repaint | magenta diff) plus both raw frames. Any difference is a primitive whose
recorded bbox/hash does not cover what it draws (stale pixels on the watch).

`tools/nos_selftest_bug.txt` with `--selftest` demonstrates a caught bug
(`set selftest_bug 1` draws a dot that is never recorded).

## Self-test screen

`--selftest` boots `sim/nos_selftest.c` instead of `nos_apps_boot()`: an orbiting dot, a
ticking seconds counter, a disc that follows the finger, tap = push a scrolling list
(TX_PUSH), drag up from the bottom = pull it (TX_RISE), long press = toast, air gesture =
banner. It uses only core primitives, so it works when apps are broken.

## Files

| File | Role |
|---|---|
| `sim/nos_sim.c` | main, window, 30 fps loop, script runner, cost model, damage check |
| `sim/nos_hal_sim.c` | `core/hal.h` on the PC: clocks, RTC, store, power, fake radios/sensors |
| `sim/nos_img.c` | PNG (own deflate), streaming GIF (LZW, NOS palette), contact sheets |
| `sim/nos_sim.h` | interface between the sim files (`nos_ext_sim_set`, clock, RTC, images) |
| `sim/nos_selftest.c` | debug root screen |
| `sim/nos_ext_sim.c` | `nos/hal_ext.h` on the PC (modules agent) + the sim hooks below |
| `core/gfx_cost.h` | cost hooks |
| `tools/nos_tour.txt` | every screen + main motions (`--shots-all`) |
| `tools/nos_smoke.txt` | quick end-to-end check |
| `tools/nos_shots.py` | report / diff / late helpers |
| `tools/gfx_cost_exact.c` | pixel-exactness and accuracy test for the hooks |

### For sim/nos_ext_sim.c (modules)

The sim calls `hal_ext_init()` once at boot and `nos_ext_sim_tick(hal_millis())` before every
frame, and forwards unknown `set KEY VALUE` lines to `int nos_ext_sim_set(const char *key,
const char *value)` (return 1 if handled). nos_ext_sim.c must define these three,
`hal_ext_sim_present` and every function in `nos/hal_ext.h` (including `hal_haptic`, which
`core/hal.h` also declares, and `hal_button_down`, which returns `sim_button_down` from
`nos_sim.h`). Use `hal_millis()` for time so scripted runs stay deterministic.
