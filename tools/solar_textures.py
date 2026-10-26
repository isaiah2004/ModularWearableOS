"""solar_textures.py - pack Solar System Scope 2k textures (CC BY 4.0, download from solarsystemscope.com/textures) into docs/assets/solar/textures.js
as 1024x512 JPEG data URIs (data URIs keep canvas pixels readable from file://). usage: python tools/solar_textures.py <dir with 2k_*.jpg> docs/assets/solar/textures.js"""
import base64, io, os, sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
names = {'sun':'2k_sun.jpg','mercury':'2k_mercury.jpg','venus':'2k_venus_atmosphere.jpg','venus_surface':'2k_venus_surface.jpg',
         'earth':'2k_earth_daymap.jpg','earth_night':'2k_earth_nightmap.jpg','moon':'2k_moon.jpg','mars':'2k_mars.jpg',
         'jupiter':'2k_jupiter.jpg','saturn':'2k_saturn.jpg','uranus':'2k_uranus.jpg','neptune':'2k_neptune.jpg'}
lines = ['/* Planet textures: Solar System Scope (solarsystemscope.com/textures), CC BY 4.0, based on NASA imagery.',
         ' * Downscaled to 1024x512 (the size the watch would hold in PSRAM as RGB565: 1 MB each). */', 'const SOLAR_TEX = {']
tot = 0
for k, f in names.items():
    im = Image.open(os.path.join(src, f)).convert('RGB').resize((1024, 512), Image.LANCZOS)
    b = io.BytesIO(); im.save(b, 'JPEG', quality=80, optimize=True); tot += b.tell()
    lines.append(f"  {k}: 'data:image/jpeg;base64,{base64.b64encode(b.getvalue()).decode()}',")
ring = Image.open(os.path.join(src, '2k_saturn_ring_alpha.png')).convert('RGBA').resize((512, 32), Image.LANCZOS)
b = io.BytesIO(); ring.save(b, 'PNG', optimize=True); tot += b.tell()
lines.append(f"  saturn_ring: 'data:image/png;base64,{base64.b64encode(b.getvalue()).decode()}',")
lines.append('};')
open(out, 'w').write('\n'.join(lines) + '\n')
print('bytes', tot, 'file', os.path.getsize(out))
