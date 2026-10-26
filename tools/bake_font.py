"""Bake TTF fonts into C alpha-atlas arrays for the watch renderer.

Usage:
    python bake_font.py          regenerates core/fonts/ (system TTFs, original set)
    python bake_font.py --nos    regenerates nos/fonts/ (OFL fonts in third_party/fonts)

Each font becomes a font_t (core/font.h) with sorted glyph metrics and a packed
8-bit alpha bitmap. Tabular fonts give every digit the same advance (ink centred)
so a ticking value doesn't jitter. NOS specs may also set a variable-font weight
(`weight`, wght axis) and `tracking` (px added to every advance: letter-spacing
baked in, for the ALL-CAPS mono labels).
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "core", "fonts")
NOS_OUT = os.path.join(ROOT, "nos", "fonts")
FONTS_DIR = r"C:\Windows\Fonts"
TP = os.path.join(ROOT, "third_party", "fonts")

# ASCII plus a few typographic extras: degree (0xB0) and middot (0xB7)
ASCII = "".join(chr(c) for c in range(32, 127)) + "°·"

# reduced charset for the Infograph day/date (uppercase months + weekdays + digits)
DATECS = "0123456789/ ABCDEFGHIJKLMNOPQRSTUVWXYZ"

# px sizes = 240-grid design size x (466/240), matching GFX_S() in gfx.h
SPECS = [
    # (c_name, ttf file, px size, charset, tabular_digits)
    ("font_big",    "segoeuib.ttf", 101, "0123456789:.°", True),
    ("font_large",  "seguisb.ttf",  58,  ASCII,           False),
    ("font_medium", "seguisb.ttf",  43,  ASCII,           False),
    ("font_small",  "segoeui.ttf",  25,  ASCII,           False),
    # Infograph hero column (dedicated sizes so other screens are unaffected):
    # ~15% larger than big/medium, date a touch up from small.
    ("font_clock",  "segoeuib.ttf", 116, "0123456789",    True),
    ("font_dow",    "seguisb.ttf",  50,  DATECS,          False),
    ("font_date",   "seguisb.ttf",  31,  DATECS,          False),
]

# ---- NOS (Nothing-inspired OS) type system -------------------------------------
# Grotesk: Space Grotesk (OFL), stand-in for Nothing's NType 82.
# Mono:    Space Mono (OFL), stand-in for Lettera Mono, for ALL-CAPS labels.
# Sizes are em px on the 466 px / 326 ppi panel (12.8 px per mm). Both families
# have a 0.70 em cap height.
SG = os.path.join(TP, "spacegrotesk", "SpaceGrotesk[wght].ttf")
SM = os.path.join(TP, "spacemono", "SpaceMono-Regular.ttf")
HERO_CS = "0123456789:"
NOS_SPECS = [
    # (c_name, ttf, em px, charset, tabular, weight, tracking px, note)
    ("nf_hero",    SG, 160, HERO_CS, True,  400, -4, "full-screen clock, cap 112 px (8.7 mm); HH:MM is 432 px wide"),
    ("nf_display", SG, 64,  ASCII,   True,  500, 0, "big values, cap 45 px (3.5 mm)"),
    ("nf_title",   SG, 32,  ASCII,   False, 500, 0, "screen titles, cap 22 px (1.75 mm)"),
    ("nf_body",    SG, 26,  ASCII,   False, 400, 0, "body text, cap 18 px (1.4 mm), x-height 13 px"),
    ("nf_label",   SM, 18,  ASCII,   False, None, 1, "ALL-CAPS labels, cap 13 px, +0.06 em tracking"),
    ("nf_small",   SM, 15,  ASCII,   False, None, 1, "ALL-CAPS micro text, cap 10.5 px, +0.07 em tracking"),
]


def load_font(ttf, size, weight=None):
    path = ttf if os.path.isabs(ttf) else os.path.join(FONTS_DIR, ttf)
    font = ImageFont.truetype(path, size)
    if weight is not None:
        font.set_variation_by_axes([weight])
    return font


def bake(c_name, ttf, size, charset, tabular, weight=None, tracking=0,
         out_dir=OUT, include='"../font.h"', note=""):
    font = load_font(ttf, size, weight)
    ascent, descent = font.getmetrics()
    pad = max(16, size // 4)

    advances = {ch: int(round(font.getlength(ch))) for ch in charset}
    if tabular:
        digits = [d for d in "0123456789" if d in advances]
        digit_adv = max(advances[d] for d in digits)
        for d in digits:
            advances[d] = digit_adv

    glyphs = []
    bitmap = bytearray()

    for ch in sorted(set(charset), key=ord):
        adv = advances[ch] + tracking
        canvas = Image.new("L", (advances[ch] + 2 * pad, ascent + descent + 2 * pad), 0)
        # draw.text with default anchor puts the top of the ascent box at y
        ImageDraw.Draw(canvas).text((pad, pad), ch, font=font, fill=255)
        box = canvas.getbbox()
        if box is None:  # space etc.
            glyphs.append((ord(ch), 0, 0, 0, 0, adv, len(bitmap)))
            continue
        ink = canvas.crop(box)
        w, h = ink.size
        xoff = box[0] - pad
        yoff = box[1] - pad - ascent  # relative to baseline
        if tabular and ch.isdigit():
            xoff = (advances[ch] - w) // 2  # center ink in the uniform advance
        xoff += tracking // 2             # keep tracked glyphs centred in their advance
        if not (w <= 255 and h <= 255 and -128 <= xoff <= 127 and -128 <= yoff <= 127 and adv <= 255):
            raise ValueError(f"{c_name} '{ch}': metrics overflow font_glyph fields "
                             f"(w={w} h={h} xoff={xoff} yoff={yoff} adv={adv})")
        off = len(bitmap)
        bitmap += ink.tobytes()
        glyphs.append((ord(ch), w, h, xoff, yoff, adv, off))

    if ascent + descent > 255:
        raise ValueError(f"{c_name}: line height {ascent + descent} > 255")
    src = os.path.basename(ttf) + (f" wght {weight}" if weight else "")
    lines = []
    lines.append(f"#include {include}\n")
    lines.append(f"/* generated by tools/bake_font.py from {src} @ {size}px"
                 + (f", tracking +{tracking}px" if tracking else "")
                 + (f" -- {note}" if note else "") + " */\n")
    lines.append(f"static const uint8_t {c_name}_bitmap[] = {{")
    for i in range(0, len(bitmap), 24):
        lines.append("    " + ",".join(str(b) for b in bitmap[i:i + 24]) + ",")
    lines.append("};\n")
    lines.append(f"static const font_glyph {c_name}_glyphs[] = {{")
    for code, w, h, xo, yo, adv, off in glyphs:
        lines.append(f"    {{ {code}, {w}, {h}, {xo}, {yo}, {adv}, {off} }},")
    lines.append("};\n")
    lines.append(f"const font_t {c_name} = {{ {c_name}_glyphs, {len(glyphs)}, "
                 f"{c_name}_bitmap, {ascent + descent}, {ascent} }};")

    path = os.path.join(out_dir, c_name + ".c")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    size_b = len(bitmap) + 10 * len(glyphs)
    print(f"{c_name}: {len(glyphs)} glyphs, {len(bitmap)} bytes bitmap -> {path}")
    return size_b


NOS_HEADER = """/* nos_fonts.h - NOS proportional type (generated by tools/bake_font.py --nos).
 *
 * House pairing: Space Grotesk (grotesk; NType 82 stand-in) for numbers, titles and
 * body, Space Mono (Lettera Mono stand-in) for ALL-CAPS labels. Both SIL OFL 1.1,
 * sources + licences in third_party/fonts/. Draw with font_draw() (core/font.h):
 * pen at (x, baseline). The degree sign is the single byte "\\xB0" (Latin-1), as in
 * core/fonts; "\\xB7" is a middot.
 *
 * Sizes are em px on the 466 px, 326 ppi panel (12.8 px/mm):
{notes}
 * Digits are tabular in nf_hero and nf_display (ticking values never jitter).
 * nf_label / nf_small have letter-spacing baked into their advances. */
#ifndef NOS_FONTS_H
#define NOS_FONTS_H
#include "font.h"

{externs}
#endif
"""


def bake_nos():
    os.makedirs(NOS_OUT, exist_ok=True)
    total = 0
    notes, externs = [], []
    for c_name, ttf, size, cs, tab, weight, track, note in NOS_SPECS:
        total += bake(c_name, ttf, size, cs, tab, weight, track, NOS_OUT, '"font.h"', note)
        fam = "Space Grotesk" if ttf == SG else "Space Mono"
        wt = {300: "Light", 400: "Regular", 500: "Medium", 700: "Bold"}.get(weight, "Regular")
        notes.append(f" *   {c_name:<10} {fam} {wt} {size} px: {note}")
        externs.append(f"extern const font_t {c_name};")
    with open(os.path.join(NOS_OUT, "nos_fonts.h"), "w", encoding="ascii") as f:
        f.write(NOS_HEADER.format(notes="\n".join(notes), externs="\n".join(externs)))
    print(f"NOS fonts: ~{total / 1024:.0f} KB flash")


if __name__ == "__main__":
    if "--nos" in sys.argv:
        bake_nos()
    else:
        os.makedirs(OUT, exist_ok=True)
        for spec in SPECS:
            bake(*spec)
