"""Round 3: reproducible a684695 A/B, no device I/O or Windows error dialogs."""
from pathlib import Path
import argparse, csv, ctypes, importlib.util, io, json, os, statistics, subprocess, tarfile
ROOT=Path(__file__).resolve().parents[2]
os.chdir(ROOT)
if os.name=='nt': ctypes.windll.kernel32.SetErrorMode(0x8003)
os.environ['PATH']='C:/mingw64/bin;'+os.environ['PATH']
spec=importlib.util.spec_from_file_location('builder',ROOT/'tools/orrery/ringfix/review.py')
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
OUT=ROOT/'build/orrery/scrub3';OUT.mkdir(parents=True,exist_ok=True);b.OUT=OUT
SCRIPTS=['scrub_exact','scrub_board','month_scrub','arc_motion']
def snapshot():
    dest=OUT/'baseline'
    if not (dest/'nos/faces/face_orrery.c').exists():
        data=subprocess.check_output(['git','archive','a684695','core','nos','sim','firmware/main'],stderr=subprocess.PIPE)
        with tarfile.open(fileobj=io.BytesIO(data)) as ar:
            for m in ar.getmembers():
                if not m.isfile():continue
                p=(dest/m.name).resolve();assert p.is_relative_to(dest.resolve())
                p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(ar.extractfile(m).read())
    # Identical pricing on both sides; no renderer/astronomy optimization is
    # transplanted into a684695. Count its original double operations.
    for name in ['core/gfx_cost.h','sim/nos_sim.c']:
        (dest/name).write_bytes((ROOT/name).read_bytes())
    original=subprocess.check_output(['git','show','a684695:core/ephem.c']).decode()
    current=(ROOT/'core/ephem.c').read_text()
    hooks=current[current.index('#include "gfx_cost.h"'):current.index('#define D2R')]
    (dest/'core/ephem.c').write_text(original.replace('#define D2R',hooks+'#define D2R',1))
    original=subprocess.check_output(['git','show','a684695:nos/faces/face_orrery.c']).decode()
    original=original.replace('    orr_prof_frame();','    orr_prof_frame();\n#ifdef GFX_COST_MODEL\n    gfx_cost_orr_math_scope(1);\n#endif')
    original=original.replace('    S.jd_prev_frame = S.jd;','    S.jd_prev_frame = S.jd;\n#ifdef GFX_COST_MODEL\n    gfx_cost_orr_math_scope(0);\n#endif')
    (dest/'nos/faces/face_orrery.c').write_text(original)
    return dest
def summary(dest):
    rows=list(csv.DictReader((dest/'frames.csv').open()))
    out={}
    for sec in dict.fromkeys(r['section'] for r in rows):
        group=[r for r in rows if r['section']==sec and int(r['dirty_px'])]
        if not group:continue
        work=[float(r['est_frame_ms']) for r in group]
        out[sec]={'frames':len(group),'work_mean':statistics.mean(work),'work_median':statistics.median(work),'work_max':max(work),
            'cpu_mean':statistics.mean(float(r['est_raster_ms']) for r in group),
            'math_mean':statistics.mean(float(r.get('ms_orr_math',0)) for r in group),
            'categories':{k:statistics.mean(float(r[k]) for r in group) for k in group[0] if k.startswith('orr_')}}
    return out
def verify():
    perf={}
    for name in ['before','after']:
        rows=[r for r in csv.DictReader((OUT/f'{name}_scrub_exact/frames.csv').open()) if r['section'].startswith('scrub') and int(r['dirty_px'])]
        work=[float(r['est_frame_ms']) for r in rows]
        perf[name]={'frames':len(work),'work_mean':statistics.mean(work),'work_median':statistics.median(work),'work_max':max(work)}
    # Round 4 retains +2.84 ms of measured residual on both sides.
    assert abs(perf['before']['work_mean']-(47.4+2.84))<.05,perf['before']
    perf['target_ms']=33;perf['target_met']=perf['after']['work_max']<=33
    (OUT/'performance.json').write_text(json.dumps(perf,indent=2))
    print('PASS baseline calibration; '+('TARGET MET' if perf['target_met'] else 'TARGET MISS'),perf,flush=True)
    for script in SCRIPTS:
        assert (OUT/f'after_{script}.hash').read_bytes()==(OUT/f'nocost_{script}.hash').read_bytes(),script
        for png in (OUT/f'before_{script}').glob('*.png'):
            if 'drag' not in png.name:
                assert png.read_bytes()==(OUT/f'after_{script}'/png.name).read_bytes(),png
        assert (OUT/f'before_{script}.hash').read_text().splitlines()[-1].split()[1]==(OUT/f'after_{script}.hash').read_text().splitlines()[-1].split()[1],script
    print('PASS settled a684695 PNGs/final hashes; all cost/no-cost frame hashes',flush=True)
    for mode in ['after','nocost']:
        obj=OUT/f'obj_{mode}';sim=OUT/f'sim_{mode}.o'
        b.command(['C:/mingw64/bin/objcopy.exe','--redefine-sym','main=sim_program_main',obj/'sim_nos_sim.c.o',sim],OUT/f'objcopy_{mode}.log')
        objects=[p for p in obj.glob('*.o') if p.name not in ['sim_nos_sim.c.o','nos_faces_face_orrery.c.o']]
        flags=['-O2','-std=c99','-DNOS_PSRAM_TRACKING','-Icore','-Inos','-Inos/fonts','-Isim']+(['-DGFX_COST_MODEL'] if mode=='after' else [])
        exe=OUT/f'unit_{mode}.exe'
        b.command([b.GCC,*flags,'tests/orrery_raster_test.c',*objects,sim,'-o',exe,'-lgdi32','-luser32','-lwinmm','-lm'],OUT/f'unit_build_{mode}.log')
        b.run(exe,ROOT/'tools/orrery/perf_round7.txt',f'unit_{mode}','--no-media')
        print('PASS renderer unit regressions',mode,flush=True)
    for script in (ROOT/'tools/orrery').glob('*.txt'):
        b.run(OUT/'after.exe',script,'all_'+script.stem,'--no-media')
    print('PASS all ORRERY scripts',flush=True)
    for script in ['scrub_exact','arc_motion']:
        clean=OUT/f'damage_{script}.txt'
        clean.write_text('\n'.join(s for s in (ROOT/f'tools/orrery/{script}.txt').read_text().splitlines() if not s.startswith('expect '))+'\n')
        b.command([OUT/'after.exe','--damagecheck',clean,'--out',OUT/f'damage_{script}','--quiet'],OUT/f'damage_{script}.log')
    print('PASS forced-full versus damaged output',flush=True)
    # The standalone astronomy test uses printed checks, not CRT assert.
    b.command([b.GCC,'-O2','-std=c99','-Icore','tests/ephem_test.c','core/ephem.c','-o',OUT/'ephem.exe','-lm'],OUT/'ephem_build.log')
    b.command([OUT/'ephem.exe'],OUT/'ephem.log')
    print('PASS ephemeris reference vectors/events',flush=True)
    cc=next(Path('C:/Users/isaia/.espressif/tools/xtensa-esp-elf').rglob('xtensa-esp-elf-gcc.exe'))
    config=cc.parent.parent/'lib/xtensa_esp32s3.so';sizes={}
    for kind,source in [('before',OUT/'baseline'),('after',ROOT)]:
        probe=OUT/f'size_{kind}.c';obj=probe.with_suffix('.o')
        types={'state':'orr_state','list':'orr_list','orbit':'orr_orbit_cache','profile':'orr_prof'}
        if kind=='after':types['plate']='orr_plate_mask'
        else:types['hot']='orr_hot'
        probe.write_text(f'#include "{(source/"nos/faces/face_orrery.c").as_posix()}"\n'+''.join(f'unsigned char measure_{n}[sizeof({t})];\n' for n,t in types.items()))
        b.command([cc,f'-mdynconfig={config}','-O2','-std=c99','-Wall','-Wextra','-Wno-unused-parameter','-fstack-usage',
            *(f'-I{source/p}' for p in ['core','nos','nos/fonts','sim']),'-c',probe,'-o',obj],OUT/f'target_{kind}.log')
        symbols=b.command([cc.with_name('xtensa-esp-elf-nm.exe'),'-S',obj],OUT/f'target_symbols_{kind}.log')
        sizes[kind]={line.split()[-1][8:]:int(line.split()[1],16) for line in symbols.splitlines() if ' measure_' in line}
    (OUT/'memory.json').write_text(json.dumps(sizes,indent=2))
    b.command([cc,f'-mdynconfig={config}','-O2','-std=c99','-Icore','-c','core/ephem.c','-o',OUT/'target_ephem.o'],OUT/'target_ephem.log')
    print('PASS ESP32-S3 face and astronomy compile; sizes',sizes,flush=True)
def main():
    p=argparse.ArgumentParser();p.add_argument('--baseline',action='store_true');p.add_argument('--nocost',action='store_true');p.add_argument('--verify',action='store_true');a=p.parse_args()
    if a.verify:verify();return
    src=snapshot() if a.baseline else ROOT
    name='before' if a.baseline else 'nocost' if a.nocost else 'after'
    exe=b.build(name,src,not a.nocost)
    results={}
    for script in SCRIPTS:
        dest=b.run(exe,ROOT/f'tools/orrery/{script}.txt',f'{name}_{script}','--hashes',OUT/f'{name}_{script}.hash')
        results[script]=summary(dest)
    (OUT/f'{name}.json').write_text(json.dumps(results,indent=2))
    print(json.dumps({k:{s:v for s,v in data.items() if 'scrub' in s} for k,data in results.items()},indent=2))
if __name__=='__main__':main()
