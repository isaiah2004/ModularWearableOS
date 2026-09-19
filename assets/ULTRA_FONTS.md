# Ultra font assets

Saira is by the Saira Project Authors / Omnibus-Type, under SIL OFL 1.1.
The license is included in [Saira-OFL.txt](Saira-OFL.txt).

Source downloaded 2026-09-14 from the
[Google Fonts repository](https://github.com/google/fonts/tree/main/ofl/saira):
`Saira[wdth,wght].ttf`, saved here as `Saira-Variable.ttf`.

Source SHA-256:
`9d050fc5a01c85f74c4257c207d50b55d1e40c37308c642f974a2c5003231dde`

| Generated asset | Weight | Width | Embedded bytes |
|---|---:|---:|---:|
| Saira-Bold.ttf | 700 | 100 | 16,436 |
| SairaCondensed-SemiBold.ttf | 600 | 75 | 16,532 |

Both are fully static TrueType instances, subset to printable ASCII
U+0020–U+007E with the relevant outlines, metrics, kerning and license metadata.
Only these instances are embedded; the variable font is a build source. Its
default weight is **100 (Thin)**, so embedding it directly would not produce
Bold with stb_truetype.

Regenerate the TTFs and `core/font_saira_*_ttf.h` headers using Python with
`fonttools==4.65.0` installed:

```powershell
python tools/bake_ultra_fonts.py
```

The script uses fontTools'
[static instancer](https://fonttools.readthedocs.io/en/latest/varLib/instancer.html),
pins both axes, updates font names, subsets glyphs and preserves source
timestamps for repeatable output. Normal firmware/simulator builds need only
the checked-in headers, not Python or fontTools.
