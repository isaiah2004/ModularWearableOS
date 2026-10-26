"""Reproducible 4e25f20/final captures, regression runs and cost comparison.

Build final cost/no-cost executables with sim/nos_build.ps1 first. Only reads
Git history; baseline sources/objects and all output live under build/.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import csv
import ctypes
import json
import io
import os
import shutil
import statistics
import subprocess
import tarfile
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[3]
os.chdir(ROOT)
OUT = ROOT / 'build/orrery/ringfinal'
OUT.mkdir(parents=True, exist_ok=True)
if os.name == 'nt':
    # Inherited by every child, including the unchanged historical executable.
    ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
GCC = shutil.which('gcc') or 'C:/mingw64/bin/gcc.exe'
FINAL = ROOT / 'build/nos_sim_ringfinal.exe'
NO_COST = ROOT / 'build/nos_sim_ringfinal_nocost.exe'
BEFORE = ROOT / 'build/nos_sim_ringbefore.exe'


def command(args, log):
    p = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    log.write_text(p.stdout + p.stderr, encoding='utf-8')
    if p.returncode:
        raise RuntimeError(f'{args[0]} exit {p.returncode}: {log}')
    return p.stdout


def baseline():
    """Rebuild every baseline source from Git, without touching the worktree."""
    dest = OUT / 'baseline-source'
    # Capture benign Git stderr too: Windows PowerShell 5 otherwise turns it
    # into a terminating error in render.ps1 even when Git exits successfully.
    data = subprocess.check_output(['git', 'archive', '4e25f20', 'core', 'nos', 'sim', 'firmware/main'],
                                   stderr=subprocess.PIPE)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for member in archive.getmembers():
            if not member.isfile() or Path(member.name).suffix not in ('.c', '.h', '.inc'):
                continue
            path = (dest / member.name).resolve()
            assert path.is_relative_to(dest.resolve())
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.extractfile(member).read())
    core = 'gfx font sprite anim notif battery pedometer walk motion_wake wake_service knock ephem'.split()
    srcs = [*sorted((dest/'nos').rglob('*.c')),
            *(dest/'core'/f'{n}.c' for n in core),
            *sorted((dest/'core/fonts').glob('*.c')),
            *sorted((dest/'sim').glob('nos_*.c'))]
    flags = ['-O2', '-std=c99', '-DGFX_COST_MODEL', '-D_WIN32_WINNT=0x0600',
             *(f'-I{dest / p}' for p in ['core', 'nos', 'nos/fonts', 'sim'])]
    def compile_one(src):
        obj = src.with_suffix('.o')
        command([GCC, *flags, '-c', src, '-o', obj], src.with_suffix('.log'))
        return obj
    with ThreadPoolExecutor(max_workers=8) as pool:
        objects = list(pool.map(compile_one, srcs))
    command([GCC, *objects, '-o', BEFORE, '-lgdi32', '-luser32', '-lwinmm', '-lm'], OUT/'baseline-build.log')
    print(f'Built baseline 4e25f20: {len(srcs)} sources', flush=True)


def run(exe, script, name, *extra):
    dest = OUT / name
    command([exe, '--face', '8', '--script', script, '--out', dest, '--quiet', *extra],
            OUT / f'{name}.log')
    return dest


def summary(dest):
    rows = list(csv.DictReader((dest/'frames.csv').open()))
    result = {}
    for section in dict.fromkeys(r['section'] for r in rows):
        group = [r for r in rows if r['section'] == section]
        values = sorted(float(r['est_frame_ms']) for r in group)
        result[section] = dict(frames=len(group), sent=sum(int(r['dirty_px']) > 0 for r in group),
                               late=sum(int(r['late']) for r in group),
                               p50_ms=statistics.median(values),
                               p95_ms=values[int(.95*(len(values)-1))], max_ms=max(values),
                               max_commands=max(int(r['cmds']) for r in group))
    return result


def images():
    fontpath = Path('C:/Windows/Fonts/consola.ttf')
    font = ImageFont.truetype(str(fontpath), 22) if fontpath.exists() else ImageFont.load_default()
    sheet = Image.new('RGB', (4*486+20, 2*556+20), '#202124')
    draw = ImageDraw.Draw(sheet)
    for row, kind in enumerate(['before', 'after']):
        draw.text((20, row*556+10), '4e25f20 BEFORE' if not row else 'FINAL AFTER', font=font, fill='white')
        for col, (shot, label) in enumerate([('now','NOW'), ('travelled','TRAVELLED +99 D'),
                                           ('drag','MID-DRAG'), ('tilt','3D TILT')]):
            x, y = 20+col*486, 50+row*556
            im = Image.open(OUT/kind/f'style5_{shot}.png').convert('RGB')
            assert im.size == (466,466)
            sheet.paste(im, (x,y))
            draw.text((x,y+472), label, font=font, fill='white')
        im = Image.open(OUT/kind/'style5_now.png')
        # Include both side flip points and the selected values near noon.
        im.crop((0,0,466,325)).resize((932,650), Image.Resampling.NEAREST).save(OUT/f'labels_{kind}_2x.png')
    sheet.save(OUT/'sheet.png')
    for name in ['r1_aod','r2_wake_locked']:
        a = Image.open(OUT/'before_locked'/f'{name}.png').convert('RGB')
        b = Image.open(OUT/'after_locked'/f'{name}.png').convert('RGB')
        assert a.tobytes() == b.tobytes(), f'{name} changed'
    print('PASS AOD and locked captures byte-identical', flush=True)


def resting_costs():
    script = OUT/'resting.txt'
    script.write_text('face 8\nringstyle 5\ndate 2026-10-11\ntime 10:08:15\nwait 2500\n'
                      'hold 300 300 1700\nwait 700\ntap 380 75\nwait 1200\n'
                      'mark rings_rest\nwait 1000\nexpect idle\nexpect nolate\n')
    return {kind: summary(run(exe, script, kind+'_rest', '--no-media'))['rings_rest']
            for kind, exe in [('before', BEFORE), ('after', FINAL)]}


def main():
    baseline()
    for kind, exe in [('before', BEFORE), ('after', FINAL)]:
        run(exe, ROOT/'tools/orrery/ringread/style5.txt', kind)
        run(exe, ROOT/'tools/orrery/regress.txt', kind+'_locked')
    images()
    scripts = sorted((ROOT/'tools/orrery').rglob('*.txt'))
    for script in scripts:
        name = 'check_' + '_'.join(script.relative_to(ROOT/'tools/orrery').with_suffix('').parts)
        run(FINAL, script, name, '--no-media')
        print('PASS', script.relative_to(ROOT), flush=True)
    for kind, exe in [('cost', FINAL), ('nocost', NO_COST), ('full', FINAL)]:
        args = ['--hashes', str(OUT/f'{kind}.hash'), '--no-media']
        if kind == 'full':
            args += ['--force-full']
        # Full redraw deliberately defeats idle/damage expectations. Input and
        # frame count remain identical (these two checks consume no frames).
        script = OUT/'pixels.txt'
        script.write_text('\n'.join(line for line in (ROOT/'tools/orrery/ringread/style5.txt').read_text().splitlines()
                                    if line not in ('expect idle', 'expect nolate'))+'\n')
        run(exe, script, 'pixels_'+kind, *args)
    assert (OUT/'cost.hash').read_bytes() == (OUT/'nocost.hash').read_bytes(), 'cost hooks changed pixels'
    assert (OUT/'cost.hash').read_bytes() == (OUT/'full.hash').read_bytes(), 'damage/full mismatch'
    costs = {kind: summary(OUT/kind) for kind in ['before','after']}
    for kind, value in resting_costs().items():
        costs[kind]['rings_rest'] = value
    (OUT/'costs.json').write_text(json.dumps(costs, indent=2)+'\n')
    for section in ['open','rings_rest','drag','return_now','tilt','idle']:
        print(section, {kind: costs[kind][section] for kind in costs}, flush=True)
    (OUT/'validation.json').write_text(json.dumps(dict(baseline='4e25f20', scripts=len(scripts),
        all_exit_zero=True, locked_aod_exact=True, cost_nocost_exact=True, damage_full_exact=True), indent=2)+'\n')
    print('PASS review, all scripts, cost/no-cost and damage/full hashes', flush=True)


if __name__ == '__main__':
    main()
