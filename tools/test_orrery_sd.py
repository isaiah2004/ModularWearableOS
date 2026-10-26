"""Checks the sky pack: regenerates it with skypack_c.py into a temp dir and compares with the committed files.
No device, no SD card. usage: python tools/test_orrery_sd.py"""
import re, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEAD = struct.Struct('<4sHHIIIId16s')
SPEC = {'belt.bin': (b'ORBL', 32, 'od_belt_n'), 'moons.bin': (b'ORMN', 88, 'od_moons_n')}


def main():
    src = (ROOT / 'nos/faces/orrery_data.c').read_text(encoding='utf-8')
    jd = float(re.search(r'od_pack_jd\s*=\s*([0-9.]+)', src).group(1))
    counts = {n: int(re.search(name + r'\s*=\s*(\d+)', src).group(1)) for n, (_, _, name) in SPEC.items()}
    per = [int(x) for x in re.search(r'od_moon_count\[OD_PLANETS\]\s*=\s*\{([^}]*)\}', src).group(1).split(',')]
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        subprocess.run([sys.executable, str(ROOT / 'tools/skypack_c.py'), '--out', str(t / 'orrery_data.c'),
                        '--sd-out', str(t / 'sky'), '--no-stage'], cwd=ROOT, check=True, capture_output=True)
        if (t / 'orrery_data.c').read_bytes().replace(b'\r\n', b'\n') != (ROOT / 'nos/faces/orrery_data.c').read_bytes().replace(b'\r\n', b'\n'):
            raise SystemExit('FAIL orrery_data.c differs from a fresh skypack_c.py run')
        for name, (magic, rec, _) in SPEC.items():
            fresh, kept = (t / 'sky' / name).read_bytes(), (ROOT / 'assets/sd/sky' / name).read_bytes()
            if fresh != kept: raise SystemExit('FAIL %s differs from a fresh skypack_c.py run' % name)
            m, ver, head, count, recsz, nbytes, crc, pjd, built = HEAD.unpack_from(kept)
            if (m, ver, head, count, recsz, pjd) != (magic, 1, HEAD.size, counts[name], rec, jd): raise SystemExit('FAIL %s header' % name)
            if nbytes != len(kept) - head or zlib.crc32(kept[head:]) != crc: raise SystemExit('FAIL %s payload/CRC' % name)
            if name == 'belt.bin' and nbytes != count * rec: raise SystemExit('FAIL belt size')
            if name == 'moons.bin':
                idx = struct.unpack_from('<%dI' % (2 * len(per)), kept, head)
                at = head + 8 * len(per)
                for p, n in enumerate(per):
                    if (idx[2 * p], idx[2 * p + 1]) != (at, n): raise SystemExit('FAIL moons index %d' % p)
                    at += n * rec
                if at != len(kept) or sum(per) != count: raise SystemExit('FAIL moons tiling')
            staged = ROOT / 'build/sd/sky' / name
            if staged.is_file() and staged.read_bytes() != kept: raise SystemExit('FAIL staged %s is stale (run tools/skypack_c.py)' % name)
    print('PASS orrery sky pack: orrery_data.c, belt.bin (%d rows), moons.bin (%d records) regenerate byte-identically; header, CRC32, index, pack JD %s' % (counts['belt.bin'], counts['moons.bin'], jd))


main()
