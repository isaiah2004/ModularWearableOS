"""Save nine enlarged inspection tiles and a 3x3 contact sheet of a dial.

Usage: python tools/ultra_review.py image.png output_dir
       python tools/ultra_review.py reference.png output_dir --reference
Reference normalization is a crop/resize for layout comparison, not a claim of
perspective correction or exact AMOLED colour matching.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--reference", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    dial = Image.open(args.image).convert("RGB")
    if args.reference:
        # Bounds on the 1471x1694 display of the supplied 1628x1874 photo.
        sx, sy = dial.width / 1471, dial.height / 1694
        box = (0, round(348 * sy), round(853 * sx), round(1368 * sy))
        dial = dial.crop(box).resize((466, 466), Image.Resampling.LANCZOS)
    dial.save(args.output / "dial.png")
    font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 18)
    tile_size, gap, heading = 420, 12, 32
    sheet = Image.new("RGB", (3 * tile_size + 4 * gap, 3 * (tile_size + heading) + 4 * gap), "#202329")
    draw = ImageDraw.Draw(sheet)
    for row in range(3):
        for col in range(3):
            box = (col * dial.width // 3, row * dial.height // 3,
                   (col + 1) * dial.width // 3, (row + 1) * dial.height // 3)
            crop = dial.crop(box)
            crop.save(args.output / f"r{row+1}c{col+1}.png")
            enlarged = crop.resize((tile_size, tile_size), Image.Resampling.NEAREST)
            x, y = gap + col * (tile_size + gap), gap + row * (tile_size + heading + gap)
            draw.text((x + 6, y + 3), f"R{row+1} C{col+1}", font=font, fill="#e1e4ea")
            sheet.paste(enlarged, (x, y + heading))
    sheet.save(args.output / "grid-3x3.png")
    print(args.output / "grid-3x3.png")


if __name__ == "__main__":
    main()
