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

- Python 3.12 and host GCC/WinLibs are required. PowerShell 5.1 and 7 use the
  same Python build engine; native subprocess exit codes are checked everywhere.
- Every build is strict, including the default. `-Strict` remains accepted for
  existing automation. A compile error stops before linking cached objects; a
  link or verification failure leaves the previous executable/manifest intact.
- Each output has its own locked cache at `build/sim-objects/<output>/`.
  Source/header content, compiler binary/version, flags and object hashes govern
  reuse; preserved mtimes cannot hide a source change. `-Clean` forces recompilation
  without deleting preexisting directories. A source race aborts publication.
- The executable's adjacent `.exe.build.json` records exact input/object hashes,
  compiler and flags. Validation rejects stale inputs, missing new sources or a
  changed binary. Unique run logs stay in `.worktrees/_audit_tmp/sim-<id>` on the
  workspace volume. Existing output/cache directories are preserved.
- Linking publishes a completed temporary executable. If a running Windows
  executable is locked, the previous file is retained in the run directory.
- `-Verify` snapshots the **current** core sources/fonts/headers, builds both
  variants from that snapshot, and requires successful compiles, executions,
  two exact-size 400-frame RGB565 outputs, equal hashes and `--accuracy` success.
  It never reads HEAD and never reuses earlier verification output.
- `-DNOS_FONT_SHIM` is added automatically until `nos/fonts/nos_fonts.h` exists.
- No weak symbols: MinGW/PE weak definitions neither act as cross-object fallbacks nor
  reliably merge with a strong definition, so every symbol has exactly one definition.

## Interactive window

```
build\nos_sim.exe [--scale 1.5] [--selftest] [--fullframe]
```

The 466 px panel at 1:1 (or `--scale`), round mask, dark bezel, and a button panel under it
(transition blur on/off, 6x slow motion, open Apps / Back to see the iris open and close,
ripple blur mode, Find Phone, ring the watch, Matrix wave, Settings, test flings at one touch
sample a frame, Home, side button, cost HUD, pause). The engine runs at exactly
the configured NOS_FPS cadence (40 fps by default, high-resolution waitable timer,
absolute deadlines); the window only repaints on
frames that sent pixels, and input is handled while waiting.

| Input | Action |
|---|---|
| mouse drag / click | touch (sampled at mouse rate, like the touch controller) |
| Space or B | side button (hold for long press, tap twice for double) |
| G | next air gesture: left, right, up, down, near, far |
| M | ripple motion blur: off / tails / tails + burst (`ui_ripple_blur`) |
| W | the phone rings the watch (Find Watch) |
| S | screenshot -> `build/nos_out/shot_HHMMSS_mmm.png` |
| R | start/stop recording -> `build/nos_out/rec_NNN.gif` |
| F | frame-cost HUD (panel on the right, outside the circle) |
| D | outline this frame's dirty rects (green; red = full repaint) |
| P / N | pause / single-step one frame |
| Esc | quit |

The HUD shows the screen on top, the estimated ESP32 frame time (red when late), raster
and bus time, dirty pixels, rects, damage commands and primitive calls, the top primitive
classes, p95/max/late over the last 120 frames, and a graph with the 25 ms target budget line (at the default 40 fps).

Settings persist in `build/nos_out/store/` (interactive only). Simulator generic
key/value saves stage a replacement allocation and a temporary file with commit
and atomic rename; failures preserve the old RAM/file value. This mirror is not
the firmware paired SD checkpoint format. Firmware durability/recovery is tested
separately by the prefs-disk fixtures and hardware profiles.

## Scripted runs

```
build\nos_sim.exe --script FILE [--out DIR] [--boot] [--selftest] [--motion]
build\nos_sim.exe --shots-all [--motion]      # tools/nos_tour.txt -> build/nos_shots/
python tools\nos_shots.py run FILE            # run + DIR/index.html
```

`--motion` runs the motion audit from the first frame (see "Motion audit" below).

The clock is **virtual**: every frame is exactly `1000/NOS_FPS` ms (25 ms by default), nothing depends on wall time,
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
| `mem` | Print current/peak bytes, blocks and allocation failures per PSRAM owner, plus cache slot counts |
| `memprewarm` | Prebake all dot masks, matching the board background prebaker for memory tests |
| `memchunk BYTES` | Limit individual tracked allocations to model fragmentation; 0 restores no limit |
| `memfail N` | Fail one tracked allocation after N successes (0 = next allocation; -1 disables) |
| `expect memowner OWNER BYTES` | Assert current bytes for an owner (e.g. `orrery_card.c 0` after closing) |
| `expect memfailures N` / `expect mempeak BYTES` | Assert cumulative failure count / peak upper bound |
| `home` / `pop` | `nos_home()` / `nos_pop()` |
| `skipboot` | pop the boot screen if it is on top |
| `home_now` | collapse to home instantly, as the host does entering AOD (`nos_home_now()`) |
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
| `gif NAME N` | the next N frames as an animated GIF, exact configured NOS_FPS timing |
| `mark NAME` | start a section: frames.csv and the summary group by it |
| `log TEXT` | print with the frame number (also `log.txt`) |
| `expect top NAME` | fail unless the top screen's `name` is NAME |
| `expect depth N` | fail unless the stack depth is N |
| `expect idle` | fail if the last frame still sent pixels |
| `expect present MASK` | fail unless `hal_ext_present()` == MASK |
| `expect nolate` | fail if any frame in the current section is estimated late |
| `motion on` / `motion off` | start / stop the motion audit (`motion.csv`, `motion.txt`; `--motion` = on from frame 1) |
| `expect motionok` | fail if any finished move that began in the current section broke its MOTION30 class budget (needs the audit on; put it after a `wait` so the moves have ended) |
| `slide X0 Y0 X1 Y1 [MS] [HOLD]` | finger at constant speed (60 Hz samples), stops dead, holds HOLD ms (default 300 / 200), lifts: the worst case for drag prediction |
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
off`, `aod 0/1`, `full 0/1` (`nos_debug_force_full`), `selftest_bug 0/1`, `predict 0/1` (drag
prediction, `nos_touch_predict`), `tx push|rise|drop open|close|both
default|legacy|steady|axis|cut` (the variant a programmatic transition uses, for A/B films;
`legacy` = the pre-MOTION30 motion), `iris_blur 0/1` (the transition edge's motion blur), `ripple_blur 0-2` (the ripples' motion blur: off, tails, tails + burst), `selftest_spin RAD_PER_S` / `selftest_smear 0-3` (the
self-test's orbiting dot and its smear: none, 1 ghost, 2 ghosts, stretch). Any other key is
offered to `nos_ext_sim_set(key, value)` in `sim/nos_ext_sim.c` (module data such as heart
rate or GPS); unknown keys fail the script.

### Touch realism

Touch samples are delivered at 60 Hz (two per frame, at the frame's midpoint and end, with
the virtual clock set to those instants), so the engine's least-squares velocity sees the
same sample spacing as the real controller (the watch samples the FT3168 every 10 ms on its
own task and hands the batch over with timestamps, `nos_touch_at`). `drag` uses ease-in-out
(velocity ~0 at release), `flick` ease-in (released at about twice the average speed: 200 px
in 120 ms leaves the finger at ~3300 px/s), `slide` constant speed with a dead stop.

The engine's velocity is a least-squares fit over the last 100 ms (>= 3 samples), zero when
the finger was still (< 2 px) for 40 ms before the release. While dragging, screens and
engine transitions see the finger predicted half a frame ahead (capped at half the last
frame's travel and 24 px; none once it stops): on a 1000 px/s drag the content leads the
raw finger by ~16 px, and a dead stop overshoots by that for one frame (`slide`). `set
predict 0` compares.

## Motion audit

`docs/MOTION30.html` Â§14 gives every kind of motion a per-frame step budget at 30 fps. The
audit measures it. Screens (and the engine) report, from `tick()`, the on-screen position
of each animated thing with

```c
nos_motion_probe(const char *name, int cls, float px);   /* nos/nos.h; nothing on the watch */
```

every frame it is on the glass, still or moving, under a stable name. `px` is what is
drawn: the rounded offset for text/sprites/rects, the float position for AA circles and
pills, a radius or an extent for growing things, ink in % (0-100) for `MC_FADE`. A thing
that only exists while it moves can give its start: report the origin once when the
animation begins (e.g. in `input()`), then the live value in `tick()`; two reports in one
frame mean "the first is the origin of a new move".

| class | for | step budget (px/frame) | checks |
|---|---|---|---|
| `MC_FINGER` | drags 1:1 with the finger | none | none (the finger is the motion) |
| `MC_MOMENTUM` | flings, snap and settle springs after a release | 64, or the release speed if faster | <= 2 creep frames |
| `MC_TRANSITION` | screen-sized programmatic moves | 64 (one bigger frame = a cut) | peak/avg, first frame, last frame, creep, 7-12 frames |
| `MC_COMPONENT` | cards, banners, toasts, sheets settling | 64, prefer 32 | as transition, 7-9 frames |
| `MC_MICRO` | toggles, pressed pills, chips, badges | 16 | as transition, 3-6 frames |
| `MC_TRACKED` | scrolls, hands, marquees, drifting art | 16 | uneven whole-pixel cadence |
| `MC_DISCRETE` | digits, values, states (should not move) | 1 | - |
| `MC_DECORATIVE` | breathing, pulses, sparkle (may run on twos) | 16 | - |
| `MC_FADE` | ink levels in % | - | <= 2 sub-level (< 3 %) steps, 3-9 frames |

Frames of one probe are grouped into **moves**: a move starts at the first frame whose step
is not ~0 and ends after two still frames or when the probe stops reporting. Each move is
judged by its worst frame:

- **OVER**: a frame over the class budget. Exactly one big frame with every other <= 20 px
  is recognised as a deliberate **CUT** (`m_cut`) and passes.
- **PEAK**: biggest step > 1.8x the average (MOTION30 asks <= 1.6x; whole-pixel rounding of
  short moves adds ~0.1, so 1.6-1.8 is reported as a lower-case warning).
- **FIRST**: frame 1 moved < 12 % of the path (a "did it start?" frame).
- **SLIVER**: the last moving frame moved < 1 px (moves of >= 20 px).
- **CREEP**: more than 2 frames moving < 1.5 px (a spring or tween crawling into place).
- Warnings, not failures: `SOFT` (component step > 32), `HOLD` (still frames inside a
  move: a stall), `UNEVEN` (tracked steps changing by >= 1 px: 1-2-1-2 shimmer), `LEN`
  (frame count outside the class range).

Frames before the last `MC_FINGER` frame of a move are the finger's and are not judged; the
rest (the release) is. Output, next to `frames.csv`:

- `motion.csv`: `frame,section,name,class,px,step`, one row per probe per frame.
- `motion.txt`: the FAIL list first, then every move by section: start frame, name, class,
  frames, travel, peak (and its frame), average, peak/avg, first-frame share, last step,
  creep frames, verdict and flags. `summary.txt` gets one line with the totals.

Probes the engine already reports: `tx.push` / `tx.rise` / `tx.drop` (the leading edge),
`tx.push.in` / `.out` / `.ink` (the shared-axis variant), `tx.iris` (radius),
`tx.dissolve` (dither %), `tx.retarget.ink`, `banner`, `toast`, and every `ui_list` as
`list.<SCREEN>` (its drawn offset; `MC_FINGER` while dragged, `MC_MOMENTUM` after).

### Worked example

A screen slides a card in on a tween. In its tick:

```c
static void music_tick(float dt)
{
    S.card_x = floorf(mtw_value(&S.card_tw) + 0.5f);        /* drawn at whole px */
    nos_motion_probe("music.card.x", MC_COMPONENT, S.card_x);
    ...
}
```

and a script films and audits it:

```
# build/nos/connect/music_motion.txt
motion on
open music
wait 900
mark swap
film! swap 14
tap 380 233
wait 600
expect motionok
```

`build\nos_sim_connect.exe --script build/nos/connect/music_motion.txt --out build/nos/connect/mm`
prints `motion audit: N moves (N judged), K FAIL -> .../motion.txt`. The engine's own banner
before MOTION30 (EASE_STANDARD in, EASE_ACCEL out) read:

```
f619   banner      component    8f   190.0 px  peak  105.0 (f619 ) avg  23.8  x4.42 first  55%  last   1.0  creep 1  FAIL OVER(f619 105>64) PEAK(x4.42)
f739   banner      component    6f   190.0 px  peak   87.0 (f744 ) avg  31.7  x2.75 first   2%  last  87.0  creep 0  FAIL OVER(f744 87>64) PEAK(x2.75) FIRST(2%) LEN(6)
```

frame 1 of the arrival jumped 105 px (55 % of the move) and the exit idled at 2 % for a
frame, then leapt 87 px. With `EASE_STEADY` over 9 frames in and a constant 6-frame exit:

```
f619   banner      component    9f   190.0 px  peak   34.0 (f619 ) avg  21.1  x1.61 first  18%  last   2.0  creep 0  ok   SOFT(34>32)
f739   banner      component    5f   158.0 px  peak   32.0 (f739 ) avg  31.6  x1.01 first  20%  last  31.0  creep 0  ok
```

(the exit's last step, off the glass, is not reported: the banner is gone). Fix every
FAIL, re-film (`film!` per-frame PNGs to judge spacing), and keep `--damagecheck` and
the cost model green. `set tx ...` and `set predict 0` give A/B runs of the engine's own
motion in the same binary.

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
  1.5 ms (record + diff). **late** = `est_frame_ms > 1000/NOS_FPS` (25 ms at 40 fps).
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
banner. It uses only core primitives plus the `ui_smear_*` helpers, so it works when apps
are broken. `set selftest_spin 6.283` spins the dot at 360 deg/s (42 px a frame) and
`set selftest_smear 1|2|3` shows the R7 smear (one ghost, two ghosts, stretch).

## Files

| File | Role |
|---|---|
| `sim/nos_sim.c` | main, window, configured-FPS loop, script runner, cost model, damage check |
| `sim/nos_hal_sim.c` | `core/hal.h` on the PC: clocks, RTC, store, power, fake radios/sensors |
| `sim/nos_img.c` | PNG (own deflate), streaming GIF (LZW, NOS palette), contact sheets |
| `sim/nos_sim.h` | interface between the sim files (`nos_ext_sim_set`, clock, RTC, images) |
| `sim/nos_selftest.c` | debug root screen |
| `sim/nos_motion.c` | the motion audit (`nos_motion_probe` -> `motion.csv`, `motion.txt`) |
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


## ORRERY CPU proxy and interval assertions

ORRERY counts legacy geometry visits in `orr_walk` and `orr_build` at 1 us each.
`orr_index` prices a retained index fetch, horizontal-bounds check and dispatch at
0.25 us. These are coarse CPU proxies; ephemerides, Kepler solving and layout remain
uncalibrated. `orr_build` is serial tick work, before any flush can overlap it.

PSRAM-target raster now adds `psram_write` (56 ns per covered pixel) and, for
read/modify/write, `psram_rmw` (744 ns per covered pixel). Thus a clear costs
4 + 56 = 60 ns/px; AA work gets an additional 800 ns/px over its internal-buffer price.
Solid circle/pill interiors pay writes only; their actual AA edge pixels pay both.
Other AA classes conservatively treat covered pixels as RMW. This is an aggregate
board-informed proxy, not a cache simulator. See [ORRERY_PERF.md](ORRERY_PERF.md).

`sprite_fb()` marks `gfx_fb.cost_psram` in cost-model builds. `gfx_row()` reports that
destination to existing pixel hooks, including text; the raw-pointer fast clear does
so explicitly. A manually constructed PSRAM framebuffer must also set `cost_psram=1`;
raw writes without raster hooks remain unpriced. Internal stripes and scratch retain
zero. Destination fields/hooks and extra edge counters compile out on the board.
PSRAM raster base costs and surcharges precede the bus pipeline, rather than being
incorrectly overlapped with a later stripe flush. This also exposes launcher prewarm
work; other screens' internal-buffer pixel prices have not changed.

- `expect idle` retains its previous last-frame meaning.
- `expect idle 5000` advances the RTC for five seconds and fails if **any** frame sends pixels.
- `expect maxdirty 20000 6000` advances six seconds and fails if **any** frame sends more than
  20,000 pixels. Without a duration, it checks the last frame. This permits a clock change
  while rejecting a full-screen repaint.

`tools/orrery/idle.txt` exercises locked real time, AOD, wake and a minute boundary.

### Effective CPU dial

Open Cost HUD (F) for the rotary dial. Drag vertically or use the wheel for 1 MHz increments across 20–480 MHz; Shift+wheel gives 10 MHz steps. Reset selects 240 MHz. The value scales CPU cost estimates while bus costs remain fixed. Pace to model controls whether interactive frame delivery waits for the predicted work duration (at least the configured frame period). Timing history resets when frequency changes.

Launch with `--cost-hud --cpu-mhz 240`; add `--no-cpu-pace` to retain timer pacing at the selected cap. Scripted runs accept `--cpu-mhz` for estimates but retain their 40 Hz virtual timeline. This is an adjustable performance approximation, not cycle-accurate emulation or hardware FPS certification.

Developer Tools is now in the launcher and simulator button panel. Its session-only switches enable Loop FPS, Frame stats, Touch position and Screen name overlays; Turn all off clears them. Direct simulator buttons also toggle FPS, stats and touch. They default off and add rendering work when enabled.

Developer Tools → Frame rate selects **24, 30, 40, 48, 60 FPS, or Unlimited** on the watch and interactive simulator. This choice is saved through the normal verified preference checkpoint and defaults to 60 FPS when upgrading older preferences. It limits frame starts, including adaptive pacing; slow frames finish normally without catch-up bursts. Unlimited removes the presentation timer limit. Animation durations retain the authored 40 Hz clock and advance using elapsed time. “Turn all off” clears overlays while retaining the frame-rate choice. Frame-rate caps do not synchronize panel scanout or guarantee tear-free output.

The effective CPU default and Reset are now **180 MHz**. The Cost HUD shows live loop FPS, painted frames/s, and modeled FPS. Low painted rate on a static scene is normal. Long status text wraps, and the HUD is wider to avoid clipping.

## Sports sessions

Build and open the dedicated Sports selector:

```powershell
.\sim\nos_build.ps1 -Strict -Out nos_sports.exe
.\build\nos_sports.exe --open sports --cost-hud
```

The simulator retains interactive workout history below its output directory's
`workouts` folder. Scripted runs use separate `workouts/run-PID` folders so a test
does not load or replace an interactive session. Pass `--workout-store DIR` to use
the same checkpoint banks across processes, and `--no-sd` to test a card-absent boot.
`--no-sd` and `--motion` also propagate to damage-check child runs.

The optional sensor replay keys are `set step_rate N` (steps/minute),
`set gps_motion N` (metres/second along a straight synthetic route, -1 for the
default park route), `set gps_fix LAT LON SPEED HDOP [AGE_MS]` (one cached fix that
ages until replaced), `set gps_auto 1`, `set hr_sample BPM QUALITY ON_WRIST`,
`set hr_age MS`, and `set hr_auto 1`. The existing `present`, `nofix`, `wrist`,
`extfail`, and `sd` keys remain available. `set units_metric 0|1` changes the
display units while the recorder keeps SI measurements.

Automation can call `workout start walk|run|cycle|hike|treadmill|workout`,
`workout pause|resume|lap|finish|discard|retry|service`, and
`expect workout FIELD MIN [MAX]`. Actions expect a return of 1 by default; append
0 to assert that an action is rejected (for example, `workout finish 0`).
`workout info LABEL` prints a JSON snapshot, and `workout history INDEX LABEL`
prints a saved/pending history entry. These commands exercise the same public
service API as the screen. Human controls, storage limits, and missing-data
semantics are described in [Sports sessions](SPORT_MODE.md).

Run the targeted engine, export, simulator, motion, and damage checks with:

```powershell
.\tools\test_workouts.ps1
```

The Sports screens render directly into internal stripes using `SCR_LIVE_RENDER`.
They skip optional full-screen PSRAM raster capture while retaining scene dimming,
directional row blur, temporal fallback, and the blurred iris transition. The
model's 30 FPS floor is 33.333 ms; its 40 FPS target is 25 ms. Passing the former
does not imply that every frame meets the latter, nor establish physical-device
timing or sensor accuracy.
