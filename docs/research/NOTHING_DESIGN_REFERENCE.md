# Nothing Design Reference (for a 466x466 round AMOLED, software-rasterized)

Research date: 2026-10-11. Nothing publishes **no public design system or official hex values**. Where a value is a
third-party reconstruction or a visual observation (not a Nothing spec), it is marked **[obs]**. Everything else is cited.

Target conversions used below: 466 px diameter, 1.43", ~326 ppi, so **1 dp ~ 2 px**. RGB565 / RGB332 values are given
because the Flat60 pipeline uses an RGB332 palette.

---

## 1. Colour

**Rule: monochrome first, red is an interrupt.** Nothing's theme is described as "a monochrome, pixelated look with some red
accents" ([NextPit](https://nextpit.com/nothing-os-2-0-review)). OS 2.0 masks every app icon white or black to "eliminate
distractions" ([Droid Life](https://www.droid-life.com/2023/07/12/5-unique-nothing-phone-2-software-features-you-might-love/),
[Android Authority](https://androidauthority.com/nothing-os-2-features-availability-3344528)). Nothing's software design lead
Bruno Viegas calls the approach "functional aesthetics": monochrome gives every app the same visual priority so apps are
"less enticing" ([Inverse](https://nc.inverse.com/tech/nothing-phone-2-software-bruno-viegas-interview)).

| Role | Hex | RGB565 | RGB332 nearest | Source |
|---|---|---|---|---|
| Background (dark) | `#000000` | `0x0000` | `0x00` | universal; OLED true black |
| Surface / card (dark) | `#111111` | `0x1082` | --* | [tokens.md](https://raw.githubusercontent.com/dominikmartn/nothing-design-skill/main/nothing-design/references/tokens.md) [obs] |
| Surface raised | `#1A1A1A` | `0x18C3` | -- | same |
| Hairline border | `#222222` / visible `#333333` | `0x2104` / `0x3186` | -- | same |
| Disabled text | `#666666` | `0x632C` | -- | same |
| Secondary text | `#999999` | `0x9CD3` | -- | same |
| Primary text | `#E8E8E8` | `0xEF5D` | -- | same |
| Display text (time) | `#FFFFFF` | `0xFFFF` | `0xFF` | same |
| **Nothing red** | **`#D71921`** | **`0xD0C4`** | **`0xC0` (#DA0000)** | [Made Good Designs](https://madegooddesigns.com/nothing-brand-guidelines/) (labelled an observed approximation) |
| Deep red (pressed) | `#B01319` | `0xB083` | `0xA0` | same |
| Cool grey | `#8A8A8E` | `0x8C51` | -- | same |
| Light mode bg / card | `#F5F5F5` / `#FFFFFF`, text `#1A1A1A` | | | tokens.md [obs] |

*RGB332 has only 2 blue bits, so true neutral greys exist only at black and white. For card greys either use the 8-bit gray
mode or pick the palette entries `#242424`-ish nearest; avoid greenish/purplish casts by testing on hardware.

**Red usage, concretely** [obs, consistent with the above sources]: at most **one red element per screen**, typically a
single dot or glyph: the analog-clock second dot, recording dot (Phone (1) has a physical red recording LED "nod to '90s
camcorders", [Engadget](https://www.engadget.com/nothing-phone-1-review-transparent-smartphone-090029611.html)), the "now"
dot in the *Life in Dots* widget ("red dots that show the present time",
[Beebom](https://gadgets.beebom.com/guides/best-nothing-os-widgets)), over-limit/alert states, destructive buttons, and a
flashlight-on tile (seen in Carl Pei's OS 3 QS teaser, [Beebom](https://beebom.com/quick-settings-redesign-nothing-phones-teased/amp/)).
Never red fills on large areas, never red text bodies. Status colours if needed: success `#4A9E5C`, warning `#D4A843` (tokens.md).

**Dark is canonical.** Light mode exists (white cards on light grey) but brand imagery, AOD and watch faces are black.
Nothing OS 4.0 shipped an "updated dark mode with enhanced contrast"
([GSMArena](https://m.gsmarena.com/nothing_os_40_announced_with_refined_design_updated_dark_mode_and_quick_settings-news-69546.php)).
On a watch: ship dark only; optional "inverted" faces (white disc, black ink) are a legitimate Nothing move.

---

## 2. Typography

**Nothing's own faces (all proprietary, do not bake):**
- **NDot (Ndot 55 / Ndot 57)** -- the dot-matrix face. Built with [Seventy Agency + Colophon Foundry](https://www.fontsinuse.com/uses/59510/nothing-phone),
  inspired by "raw digital fonts and colours from IBM's mainframe era in the 1980s". Ndot 57 is the UI face; the logo uses a
  slightly altered **Ndot 55** ([Nothing Community](https://nothing.community/d/104-ndot57-the-nothing-typeface)). Licence:
  "solely on the Nothing brand materials", no modification ([online-fonts](https://online-fonts.com/fonts/ndot-55)).
- **NType 82** (+ NType 82 Mono) -- Colophon/Seventy grotesk with "pronounced serifs, slightly thicker horizontals,
  ink-trap-like details" ([Nothing Community](https://nothing.community/d/21308-ntype-jp-font-concept)). Took over many
  NDot headline spots in OS 3.0; NDot titles survive behind *Experimental features > Dot matrix title*
  (r/NothingTech FAQ, via search excerpt; [AA leak](https://androidauthority.com/nothing-os-3-leaked-features-3478320) confirms NDot was removed from Settings headers and the lock-screen clock).
- **LL Lettera Mono** (Lineto) -- used in two widths on nothing.tech for small technical labels (fontsinuse).
- System body text is **Roboto** (OS 3 adds **Inter** as an option, [Android Faithful](https://www.androidfaithful.com/nothing-os-3-0-hands-on/)).

**What NDot looks like** [obs]: **round dots on a square grid**, no connecting strokes; caps and lowercase exist;
digits sit on a **5-wide x 7-tall** cell (the "57" naming matches the 5x7 LCD/LED convention; not officially confirmed).
Dot diameter is roughly **0.75-0.85 of pitch** (dots nearly touch, gaps clearly visible). Diagonals are stair-stepped dots,
never interpolated. Letter gap = 1 empty column; line gap = 1-2 empty rows.

**Where each is used:**
| Use | Face | Spec for this watch |
|---|---|---|
| Hero time / big number (one per screen) | dot matrix | 5x7 digits, pitch 18-24 px, dot d = 0.8 x pitch |
| Screen titles, app names | dot matrix *or* grotesk, sentence/Title case | 28-36 px |
| Body / values | grotesk | 22-28 px (11-14 dp) |
| Micro labels ("BPM", "STEPS", "MON 12") | mono, **ALL CAPS, +0.06 to +0.08 em tracking** | 16-20 px |

Discipline from the best reconstruction ([skill](https://skills.cat/skills/dominikmartn/nothing-design-skill/nothing-design)):
max **2 families, 3 sizes, 2 weights per screen**; hierarchy = display / body / ALL-CAPS mono label.

**Open-licensed substitutes (all verified HTTP 200, all SIL OFL 1.1):**
| Font | Why | Raw TTF |
|---|---|---|
| **Doto** (Oliver Lalan) | Closest NDot analogue: 6x10 dot matrix, monospace, axes **ROND 0-100** (dot roundness) and **wght 100-900** (dot size) | https://raw.githubusercontent.com/google/fonts/main/ofl/doto/Doto%5BROND%2Cwght%5D.ttf (licence: .../ofl/doto/OFL.txt, repo github.com/oliverlalan/Doto) |
| DotGothic16 (Fontworks) | Square-dot 16 px bitmap look, CJK coverage; less "Nothing" (square dots) | https://raw.githubusercontent.com/google/fonts/main/ofl/dotgothic16/DotGothic16-Regular.ttf |
| **Space Grotesk** | Quirky grotesk, nearest feel to NType 82 | https://raw.githubusercontent.com/google/fonts/main/ofl/spacegrotesk/SpaceGrotesk%5Bwght%5D.ttf |
| **Space Mono** | Labels (Lettera Mono stand-in) | https://raw.githubusercontent.com/google/fonts/main/ofl/spacemono/SpaceMono-Regular.ttf , .../SpaceMono-Bold.ttf |
| JetBrains Mono | Denser mono for numbers in lists | https://raw.githubusercontent.com/google/fonts/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf |
| Inter | Neutral body (Nothing OS 3 ships it) | https://raw.githubusercontent.com/google/fonts/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf |
| IBM Plex Mono | Matches the "IBM mainframe" lineage | https://raw.githubusercontent.com/google/fonts/main/ofl/ibmplexmono/IBMPlexMono-Regular.ttf |

Doto is variable: instance it before baking (`fonttools varLib.instancer Doto.ttf ROND=100 wght=700`).
**Recommended for the ESP32:** don't rasterize a dot TTF at all. Hand-author (or extract from Doto) a 5x7 / 6x10
bit table and draw each lit bit as a filled circle (span-fill, already in the engine). This is cheaper than glyph
blitting, scales to any pitch, gives perfect round dots, and enables per-dot animation (section 5). Keep TTF-baked
Space Grotesk / Space Mono for text.

---

## 3. Widget / card language

Observed Nothing OS widgets [obs, corroborated by [Inverse](https://www.inverse.com/input/tech/nothing-android-launcher-beta-widgets-design-phone-1-test-pixel-6), [Beebom](https://gadgets.beebom.com/guides/best-nothing-os-widgets), [Android Faithful](https://www.androidfaithful.com/nothing-os-3-0-hands-on/)]:
- **Shapes:** 2x2 rounded squares, 1x1 **full circles**, 2x1 **pills**, 4x2 wide cards. Corner radius on squares is large,
  ~**16-20% of the short side** (a 2x2 widget ~170 dp has ~28 dp corners). The circular 1x1 is a recurring Nothing
  signature (clock, battery, compass, toggles).
- **Fills:** flat dark grey (`#1A1A1A`-ish) on black, or **inverted white card with black ink** for the "hero" widget.
  No shadows, no blur, no gradients; separation by 1 px hairline or by fill contrast only.
- **Content:** one big value (dot-matrix numerals or bold grotesk) + one ALL-CAPS mono label + optional dot/segment meter.
  Examples: digital clock (24 h, dot digits), analog clock, AccuWeather weather with dot-built icons (OS 3 weather uses
  "dots of several sizes" for sun/rain/clouds, animated per condition, [Android Authority hands-on](https://www.androidauthority.com/nothing-os-3-hands-on-3488739/), [Gizmochina](https://www.gizmochina.com/2024/12/25/the-6-best-new-features-in-nothing-os-3-0/)),
  pedometer with goal ring + streak, *Life in Dots* (grid of dots = year/month/day/hour progress, red = now),
  Snake in a 2x2 dot grid, Glyph Timer (Beebom).
- **Progress = discrete units.** Use segmented bars (square-ended segments, **2 px gaps**, filled white, empty `#222`,
  over-limit red) or rows/rings of dots. Always pair with a numeric readout ([components.md](https://raw.githubusercontent.com/dominikmartn/nothing-design-skill/main/nothing-design/references/components.md)).
- **Quick-settings tiles:** OS 1/2 used **pills**; OS 3 moved to a **mix of circles and pills**, resizable 1x1 / 2x1, Bluetooth
  up to 2x2; ringer tile expands to silent/vibrate/ring ([Beebom](https://beebom.com/quick-settings-redesign-nothing-phones-teased/amp/), [Android Faithful](https://www.androidfaithful.com/nothing-os-3-0-hands-on/)).
  State [obs]: **on = white fill + black icon; off = dark grey fill + white icon**. Brightness slider is a thick pill.
- **Toggles:** pill track + circle thumb; off = `#333` track / `#666` thumb; on = white track / black thumb.
- **Buttons:** pill (radius = h/2) or technical (4-8 px); labels ALL-CAPS mono; destructive = red outline + red text.
- **Icons:** monoline, uniform stroke (~1.5 dp -> **3 px** here), round caps and joins, geometric primitives, no fills, no
  colour. Secondary style: icons **built from dots** on a grid (weather, battery, Glyph Toys). The Phone (3)
  **Glyph Matrix** is a **circular 25x25 grid of 489 LEDs** ([Yanko Design](https://www.yankodesign.com/?p=564162),
  [SDK](https://github.com/Nothing-Developer-Programme/GlyphMatrix-Developer-Kit), 0-255 brightness per LED) -- a round dot
  matrix is literally the brand's newest canvas, and maps perfectly to a round watch.

Watch-scale numbers: card radius 24-32 px, padding 24-32 px, spacing scale 8/16/24/32/48 px, touch targets >= 88 px
(44 dp), hairlines 2 px (1 dp).

---

## 4. Watch faces (9 Nothing/CMF-style layouts for 466x466 round)

The CMF Watch Pro 2 / Watch 3 Pro use "minimalist, dot-matrix-style Nothing styling", 120+ faces, each with a matching AOD
([GSMArena](https://m.gsmarena.com/cmf_watch_3_pro_review-news-70495.php), [community review](https://nothing.community/d/47268-community-review-cmf-watch-3-pro-3rd-time-lucky)).
The Watch 3 Pro panel is the **same 1.43" 466x466 AMOLED** as this project. Fan concepts:
[Nothing Watch (Ultra)](https://www.yankodesign.com/2024/10/11/the-nothing-watch-ultra-may-not-have-a-glyph-interface-but-it-makes-up-with-a-minimal-glyph-os/amp/) (centre clock + Glyph ring for battery/kcal, white bars),
[Nothing Dot Watch](https://4gnews.pt/nothing-dot-watch-e-o-conceito-de-smartwatch-nunca-antes-visto/) (LED matrix, glowing white rims),
[Garmin "Nothing Style"](https://forums.garmin.com/developer/connect-iq/f/showcase/436428/nothing-style-watchface-minimalist-dot-matrix-face) (white-on-black, one red accent: pulsing red colon once per second *or* red "1" digits, 3-letter labels STP/CAL/HRM, dot weather icons, AOD shifts pixels every 4 min).

1. **DOT MATRIX DIGITAL** -- `HH` over `MM`, 5x7 digits, pitch 22 px, dot d 18 px, digit width 110 px, gap 22 px; stacked
   block centred (~250x330 px). Date in Space Mono caps at top (`MON 12`, y=70), battery `87%` at bottom (y=400).
   Unlit dots at 8-12% white (`#1E1E1E`) to reveal the grid. Optional red colon (single-line variant `HH:MM`).
2. **GLYPH RING ANALOG** -- black disc, 60 dot ticks on r=215 (minute dots 4 px, hour dots 8 px, white 40%). Hour hand 14 px
   wide pill r=120, minute 10 px pill r=190, both white, round caps. **Second hand = one 12 px red dot** riding r=205.
   No numerals. Centre: 18 px black-cored white ring.
3. **LIFE IN DOTS** -- dot grid clipped to the circle (e.g. 12 cols x 5 rows = 60 minutes of the hour, or 24 dots = hours of the day);
   past = white, future = `#2A2A2A`, now = red. Small mono time at bottom.
4. **SEGMENT ACTIVITY** -- three concentric rings of discrete segments (steps / kcal / active min), 60 segments each, 6 px
   thick, 3 px gaps, r = 215/195/175. Centre: grotesk time 96 px + caps labels. Over-goal segments continue in red.
5. **INVERTED CARD** -- full-white disc, black dot-matrix time, black hairline ring at r=220. Reads as the "white Nothing
   phone" face. Dim to grey for AOD (never white AOD on OLED).
6. **GLYPH MATRIX MIRROR** -- whole face is a circular 25x25 dot grid (pitch 18 px, d 14 px), mimicking Phone (3)'s matrix.
   Time drawn in 5x7 digits *inside* that same grid; dot brightness levels (4 steps) for anti-aliasing-free shading.
7. **TYPOGRAPHIC SPLIT** -- left-aligned stack in a circle-safe column: huge `10` (grotesk 180 px), `42` below in `#666`,
   Space Mono caps label column at right (`HR 72 / STP 8.4K / BAT 87`). Red only on HR when out of range.
8. **COMPASS / INSTRUMENT** -- thin 2 px dial ring, 72 tick marks, cardinal letters in mono caps, red north triangle;
   digital time in small dot digits in the lower third. Echoes the Nothing compass widget / Glyph Toy compass.
9. **TAPE / RECORDER** -- two dot-grid "reels" left/right rotating with seconds, mono timecode `10:42:07` centred; nod to the
   Nothing Recorder's tape-deck look (Engadget).

AOD rule for all: same layout, strip to time + one datum, drop fills, keep only lit dots/strokes, <= 10% pixels lit,
shift 2-4 px per minute.

---

## 5. Motion

Documented behaviours:
- **Dot reveals/builds:** OS 3 teased "interactive dot animations" on the lock screen
  ([Fonearena](https://www.fonearena.com/blog/426351/nothing-os-3-0-teaser-lockscreen-customisation-interactive-dot-animations.html/amp)).
  Fingerprint animation: **dots burst outward from the touch point**; charging: **dots travel bottom to top**
  ([Android Authority leak](https://androidauthority.com/nothing-os-3-leaked-features-3478320)).
- **Weather:** per-condition dot animations (rain, sun, clouds, night) play on app open / widget tap (Gizmochina, AA).
- **Boot:** new OS 3 boot animation derived from the fingerprint dot-burst, early builds in orange (AA leak).
- **Glyph:** light patterns synced frame-accurately to ringtones; Glyph Timer/progress shows remaining time as a draining
  light strip ([MobileSyrup](https://mobilesyrup.com/2022/06/21/nothing-phone-1-glyph-interface-lights-function/), Beebom).
  Phone (3) Glyph Matrix animates toys (clock, stopwatch, battery, spin-the-bottle, compass) at low resolution
  ([9to5Google](https://9to5google.com/2025/07/01/heres-what-you-can-do-with-nothing-phone-3-new-glyph-matrix/)).

Character to reproduce: **mechanical, quantised, "click not swoosh"** -- no spring/bounce, short ease-out
(`cubic-bezier(0.25,0.1,0.25,1)`), micro 150-250 ms, transitions 300-400 ms ([tokens.md](https://raw.githubusercontent.com/dominikmartn/nothing-design-skill/main/nothing-design/references/tokens.md)).

Rasterizer-friendly recipes (60 fps, damage-limited):
- **Dot build-in:** light dots in a deterministic order (row scan, radial from centre, or seeded random) over 8-12 frames;
  each dot snaps 0 -> 100% (or 3 brightness steps), no scaling. Digit change = old dots off / new dots on with a 1-frame
  stagger per column (a "split-flap in dots").
- **Ripple:** on tap, a ring of dot brightness expands from the touch point (dot burst), 250 ms.
- **Progress fill:** segments light one at a time with a 16-33 ms tick; final segment flashes once.
- **Screen push:** hard horizontal slide with ease-out 250 ms, or a cut + dot-build; no crossfades, no blur.
- **Snap scrolling:** lists snap to rows with a short overshoot-free decel; pair every snap with a haptic tick.

---

## 6. Sound / haptic character

- Phone (1) shipped **10 ringtones each with a dedicated Glyph pattern**, described as **"lo-fi, chippy noises"** with tight
  light sync, credited to Teenage Engineering's influence ([Engadget](https://www.engadget.com/nothing-phone-1-review-transparent-smartphone-090029611.html)).
  Nothing ships "bespoke apps, fonts, sounds and wallpapers" ([nothing.tech Phone (1)](https://dk.nothing.tech/pages/phone-1)).
- Glyph Composer: 5-pad sound+light sequencer, ringtones up to 10 s; artist packs (Swedish House Mafia, Sampha) are noted as
  "less electronic" than Nothing's built-in style ([MusicRadar](https://musicradar.com/news/nothing-phone2-glyph-composer-swedish-house-mafia), [9to5Google](https://9to5google.com/2023/09/21/nothings-glyph-composer-sampha/)).
- Haptics: marketed only as "advanced vibration motors make touch responses life-like" (Phone (1) page); no documented
  haptic vocabulary. CMF watches give crown vibration feedback (GSMArena).

For this watch: short, dry, square-wave/sample-crushed blips (20-60 ms), no reverb; one crisp haptic tick (10-15 ms) per
detent, double tick for confirm, long buzz only for alarms; **sync haptic pulses with dot/segment animation frames**
the way Glyph syncs light to sound.

---

## 7. Ten signature moves

1. **True-black canvas, white ink, exactly one red dot** (`#D71921`) per screen -- and many screens with zero red.
2. **Round-dot matrix numerals** for the hero number (5x7, dot d = 0.8 pitch), with faint unlit dots showing the grid.
3. **ALL-CAPS monospace micro-labels** with wide tracking (`BPM`, `STEPS`, `MON 12`) next to big values.
4. **Discrete progress**: segmented rings/bars and dot rows, never smooth gradient arcs; overflow continues in red.
5. **Circular dot-grid canvas** echoing the Phone (3) Glyph Matrix (25x25 round grid), used for icons, toys, loaders.
6. **Monochrome monoline icons** (3 px, round caps) or dot-built icons; app icons never coloured.
7. **Flat cards**: circles, pills, and big-radius squares; on = white fill/black ink, off = dark grey; no shadows/blur/gradient.
8. **Instrument motifs**: tick rings, compass, tape reels, timecode -- engineered, "functional aesthetics".
9. **Quantised motion**: dot build-ins, outward dot bursts on touch, bottom-to-top charging dots, ease-out only.
10. **Glyph-style sync**: lo-fi chippy sounds and crisp haptic ticks timed to light/dot patterns; text in brackets for
    states (`[SAVED]`, `[LOADING...]`) instead of spinners or toasts.

---

### Provenance notes
- `#D71921`, the grey ladder and px tokens come from third-party reconstructions (Made Good Designs, dominikmartn's
  nothing-design skill), not from Nothing. They match observed UI closely enough to use, but are not official.
- Grid ratios for NDot, widget radii, QS tile colours and the analog-face red second dot are **visual observations**; no
  source documents them numerically, and a native red second-dot clock was not confirmed in any article.
- Several fetches failed (fontsinuse intermittently, Android Authority 403, Nothing Community thread bodies); the facts
  above were taken from search-result excerpts of those pages where noted.
