"""solar_rgb565.py - the planet textures as raw RGB565 for the watch's SD card.

Writes <out>/<body>.565 (1024 x 512, little-endian RGB565, 1 MB each: loaded straight into
PSRAM, no JPEG decode on the watch), earth_night.565 and saturn_ring.rgba (512 x 1 RGBA:
the ring's colour and opacity from inner to outer edge).
Source: the Solar System Scope 2k JPEGs (CC BY 4.0, solarsystemscope.com/textures).
usage: python tools/solar_rgb565.py <dir with 2k_*.jpg> <out dir>   (copy <out> to SD:/sky/)"""
import os, struct, sys
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
names = {'sun': '2k_sun.jpg', 'mercury': '2k_mercury.jpg', 'venus': '2k_venus_atmosphere.jpg', 'earth': '2k_earth_daymap.jpg',
         'earth_night': '2k_earth_nightmap.jpg', 'moon': '2k_moon.jpg', 'mars': '2k_mars.jpg', 'jupiter': '2k_jupiter.jpg',
         'saturn': '2k_saturn.jpg', 'uranus': '2k_uranus.jpg', 'neptune': '2k_neptune.jpg'}
for k, f in names.items():
    im = Image.open(os.path.join(src, f)).convert('RGB').resize((1024, 512), Image.LANCZOS)
    px = im.tobytes()
    buf = bytearray(1024 * 512 * 2)
    for i in range(1024 * 512):
        r, g, b = px[i * 3], px[i * 3 + 1], px[i * 3 + 2]
        struct.pack_into('<H', buf, i * 2, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    open(os.path.join(out, k + '.565'), 'wb').write(buf)
ring = Image.open(os.path.join(src, '2k_saturn_ring_alpha.png')).convert('RGBA').resize((512, 32), Image.LANCZOS)
open(os.path.join(out, 'saturn_ring.rgba'), 'wb').write(bytes(ring.crop((0, 0, 512, 1)).tobytes()))
print('wrote', len(names), 'textures + ring to', out)
