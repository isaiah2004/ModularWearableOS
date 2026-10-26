# ORRERY â€” the solar-system watch face

The design and its reasoning live in `docs/SOLAR_FACE.html` (the reviewed HTML prototype,
`docs/assets/solar/watch.js`). This file is the C implementation: what it does, where it
lives, and how to keep its data fresh.

## Using it

**Locked (the default: it's a watch face).** The clock sits over the Sun; the planets are where
they really are (JPL elements), tinted and lit on their sunward side; the asteroid belt, the
Kuiper belt and comets are dots; the next alignment is the one red element (lines + chip).
Every touch goes to home as on any face (swipe to the widgets, pull the sheets), except holds:

| Hold anywhere | Does |
|---|---|
| release after 0.75 s | 3D tilt on / off |
| release after 1.5 s | unlock / lock (the controls fade in, the clock gives way to the Sun) |
| still held at 3 s | the face picker (this face replaces home's 0.45 s long-press) |

A ring of dots under the finger shows the stage. A tap shows `HOLD 1.5 S TO UNLOCK`.

**Unlocked.**

| Where | Gesture | Does |
|---|---|---|
| anywhere | drag | DRAG mode: **PAN** (default, flat or 3D), **LOOK** (orbit the 3D camera), **TIME** (grab a planet and drag it round the Sun: its own year per lap; months tick, events are magnets) |
| top-right rim beside the arcs | swipe diagonally inward | the **time rings**: years / months / days dials with real dates; drag one to travel, tap a value to fly there, NOW (centre) comes home, the Ã— grip folds them |
| bottom rim 4:30-7:30 | drag along / tap | **zoom** 0.5-4x about the centre of the glass |
| right-rim arc | swipe left from the edge | the **menu** (icons): DRAG, VIEW (overhead / tilt), FILTER (planets, asteroids, comets, craft, FROM EARTH = lines to the 5 nearest), LINES (the red geometry) |
| inner / outer band, rim | tap | **zones**: inner system (true scale with the belt), giants, beyond Neptune; rim = out, core = in, top = home; edge swipe from the left = back |
| a planet, the Sun | tap | its **card** (globe PHOTO / HEAT / DOT, heat graph, moons & craft, facts) |
| a dot | tap | a one-line read-out (asteroid, comet, spacecraft) |
| the chip | tap | the **events list** (tap one: the face flies there); NOW when travelled |
| side button | press | lock |

Both openers require touch-down in the outer 22 px of the circular glass. Taps and interior-origin drags do not open them. Close the time rings before opening the menu; the rings retain ownership through their closing animation. Ring adjustments, the close grip, menu items and menu dismissal keep their existing gestures.

Sleep (always-on) locks it and brings it home to now.

## Files

| File | What |
|---|---|
| `core/ephem.c/.h` | planets (JPL Table 1), Moon, conics (asteroids, comets: ellipse / parabola / hyperbola), moons and orbiters, IAU spin axes, the alignment scan (sliced) |
| `tests/ephem_test.c` | Horizons + 7 almanac dates + prototype parity (in `tools/test.ps1`) |
| `nos/faces/face_orrery.c` | the face |
| `nos/faces/orrery_card.c` | the planet card |
| `nos/faces/orrery_events.c` | the events list |
| `nos/faces/orrery.h` | what the three share |
| `nos/faces/orrery_data.c/.h` | the small sky tables in flash and the pack date / counts (generated) |
| `nos/faces/orrery_sd.c` | loads `sky/belt.bin` and `sky/moons.bin` from the SD card through the asset worker |
| `assets/sd/sky/belt.bin`, `moons.bin` | the two big catalogs (tracked; generated, 128016 and 40616 bytes) |
| `nos/faces/face_ext.h` | `nos_face_input` (a face that takes touch), `home_open_faces` |
| `tools/skypack.py` | fetches the atlas from NASA/JPL (SBDB, Horizons, satellite elements, CAD) |
| `tools/skypack_c.py` | atlas -> `orrery_data.c`, `belt.bin`, `moons.bin` (also staged in `build/sd/sky/`) |
| `tools/test_orrery_sd.py` | regenerates the pack and checks it against the committed files, headers, CRC32 and index |
| `tools/solar_rgb565.py` | textures -> raw RGB565 for the SD card |

## Data

- **Planets, Moon, events:** computed on the watch; valid 1800-2050, no network.
- **Sky pack:** refresh every month or two (new comets, fresh moon / craft elements):
  `python tools/skypack.py --refresh`, `python tools/skypack_c.py`, rebuild, then `python tools/sdsync.py`
  to push `build/sd/sky/` to the card. The small tables stay in flash (`od_small_bodies`, `od_comets`,
  `od_crafts`, `od_flybys`, `od_pack_jd`, `od_built`, the counts). The two big catalogs are SD files,
  `<sd>/sky/belt.bin` (`ORBL`, 3999 x 32 B) and `moons.bin` (`ORMN`, 460 x 88 B, names inline, per-planet
  offset/count index). Both start with a 48-byte header (magic, version, count, record size, payload size,
  CRC32 of the payload, pack JD, build date) and are hash-bound in the build identity like the textures.
- **Belt:** nothing is baked. The first non-AOD Orrery tick asks `orrery_sd.c` for `belt.bin`; the rows are
  accepted only if the header, pack JD, count and CRC32 all match the flash tables. `belt_init` then derives
  the asteroid basis with `belt_basis_calculate()`, 500 rows per tick, publishes `S.bk` when complete and frees
  the rows. Until then the belt is simply not drawn. A failed load retries after 2 s, doubling to 60 s (a
  readable but wrong file waits 60 s). Each new failure prints one `ORRERYSD ERROR belt|moons: ...` line.
- **Moons:** the planet card with moons (not Earth) asks for `moons.bin` (read whole, 40 KB; the asset API has no
  seek) and frees it when the card closes. The "N KNOWN" counts come from flash, so they never need the card.
  While it loads the card says "MOONS LOADING"; if the file or card is missing or bad, the moons page says
  "MOON DATA MISSING / SD CARD / SKY/MOONS.BIN" instead of "0 KNOWN", and the open card refreshes itself the
  moment the data arrives.
- **No card (1.43C):** CMake embeds `assets/sd/sky/belt.bin` and `moons.bin` in the image (`NOS_SKY_EMBEDDED`);
  `orrery_sd.c` validates and serves them from flash through the same header/CRC/index checks, with no asset job.
  On the B watch the card's textures share the worker's single job slot, so a busy slot just defers the sky load
  to a later tick.
- **Textures (SD card):** `python tools/solar_rgb565.py <dir of Solar System Scope 2k JPEGs> <out>`
  and copy `<out>` to the card as `/sky/` (`sun mercury venus earth earth_night moon mars jupiter
  saturn uranus neptune .565`, `saturn_ring.rgba`; 11 MB). The sim reads `build/sd/sky/`.
  Textures: Solar System Scope, CC BY 4.0. Without the card the globes fall back to tinted spheres.

## Cost rules it follows

`draw()` runs once per internal 16-row stripe. Tick retains drawing primitives in
paint order and builds a contiguous index range for each stripe. Raster visits only
that range, checks cached horizontal bounds for narrow transition slices, and draws
straight into internal RAM. There is no ORRERY pixel canvas in PSRAM.

Geometry, labels, rings and menu have separate input caches. Each view lazily caches
AU-to-radius maps. Pure pan shifts projected points; orbit hairlines retain their
pre-pan terms to avoid cumulative rounding. Labels are reconsidered at the glass edge
and fixed control reservations. Rest sends nothing; the clock has separate minute damage.

The simulator now charges PSRAM destinations (including `sprite_fb`) for writes and
read/modify/write and puts tick construction before the transfer pipeline. All 14
scripts pass with zero estimated late frames. These are **not board timings**; astronomy
and layout are still uncalibrated. The former board result also has an unexplained
46 ms work versus 24 ms raster gap. See [ORRERY_PERF.md](ORRERY_PERF.md) for estimates,
permanent board stage timers, exact checks and remaining limitations.

## Planet card verification

The card is implemented: PHOTO / HEAT / colour DOT, NOW / FREE, heat profiles,
moons and craft, facts, body dial and vertical pages. Its buffers live in PSRAM,
textures stream from SD in tick, and draw only consumes cached state.
See [ORRERY_CARD.md](ORRERY_CARD.md) for scripts, exact simulator summaries,
render-cost assumptions, memory, prototype differences and untested hardware work.
