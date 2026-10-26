"""Local ring-scrub regression; child processes inherit disabled Windows dialogs."""
from pathlib import Path
import csv, ctypes, hashlib, importlib.util, io, json, os, shutil, statistics, subprocess, tarfile

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
if os.name == 'nt':
    ctypes.windll.kernel32.SetErrorMode(0x8003)
spec = importlib.util.spec_from_file_location('review', ROOT/'tools/orrery/ringfix/review.py')
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)
r.OUT = ROOT/'build/orrery/scrub'
r.OUT.mkdir(parents=True, exist_ok=True)

def command(args,log):
    p=subprocess.run([str(a) for a in args],capture_output=True,text=True,timeout=180)
    log.parent.mkdir(parents=True,exist_ok=True)
    log.write_text(p.stdout+p.stderr,encoding='utf-8')
    if p.returncode:raise RuntimeError(f'{args[0]} exit {p.returncode}: {log}')
    return p.stdout
r.command=command

def snapshot():
    dest = r.OUT/'baseline'
    if not (dest/'nos/faces/face_orrery.c').exists():
        data = subprocess.check_output(['git','archive','ecc74d9','core','nos','sim','firmware/main'], stderr=subprocess.PIPE)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            for member in archive.getmembers():
                if not member.isfile(): continue
                p = (dest/member.name).resolve()
                assert p.is_relative_to(dest.resolve())
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(archive.extractfile(member).read())
    return dest

def summary(dest):
    rows = list(csv.DictReader((dest/'frames.csv').open()))
    result = {}
    for section in dict.fromkeys(x['section'] for x in rows):
        group = [x for x in rows if x['section']==section]
        values = sorted(float(x['est_frame_ms']) for x in group)
        sent=[float(x['est_frame_ms']) for x in group if int(x['dirty_px'])]
        result[section] = dict(frames=len(group), sent=sum(int(x['dirty_px'])>0 for x in group),
            mean_ms=statistics.mean(values), p95_ms=values[int(.95*(len(values)-1))],
            max_ms=max(values), sent_mean_ms=statistics.mean(sent) if sent else 0,
            late=sum(int(x['late']) for x in group))
    return result

def run(kind, source=ROOT, cost=True):
    exe = r.build(kind, source, cost)
    script = r.OUT/'profile.txt'
    script.write_text((ROOT/'tools/orrery/arc_motion.txt').read_text().replace('mark open\n','set orrprof 1\nmark open\n'))
    dest = r.run(exe, script, kind, '--hashes', r.OUT/f'{kind}.hash')
    result = summary(dest)
    (r.OUT/f'{kind}.json').write_text(json.dumps(result, indent=2))
    print(kind, {k:result[k] for k in ['open','drag','coast','open_rest']}, flush=True)
    return exe

def priced_source():
    dest=r.OUT/'priced-source'
    shutil.copytree(snapshot(),dest,dirs_exist_ok=True)
    shutil.copyfile(ROOT/'sim/nos_sim.c',dest/'sim/nos_sim.c')
    src=dest/'nos/faces/face_orrery.c'
    text=src.read_text()
    old='static void scene_update(const face_ctx *c)\n{'
    assert text.count(old)==1
    src.write_text(text.replace(old,old+'\n#ifdef GFX_COST_MODEL\n'
        '    extern void gfx_cost_orr_rings(int);\n    gfx_cost_orr_rings(S.ringe>0.01f);\n#endif'))
    return dest

def unit(kind,cost=True):
    obj=r.OUT/f'obj_{kind}'
    sim=r.OUT/f'sim_test_{kind}.o';test=r.OUT/f'raster_{kind}.o';exe=r.OUT/f'raster_{kind}.exe'
    r.command([r.GCC.with_name('objcopy.exe'),'--redefine-sym','main=sim_program_main',
        obj/'sim_nos_sim.c.o',sim],r.OUT/f'objcopy_{kind}.log')
    flags=['-O2','-std=c99','-Wall','-Wextra','-Wno-unused-parameter','-Wno-format-truncation',
           '-Icore','-Inos','-Inos/fonts','-Isim']
    if cost:flags+=['-DGFX_COST_MODEL']
    r.command([r.GCC,*flags,'-c','tests/orrery_raster_test.c','-o',test],r.OUT/f'raster_build_{kind}.log')
    objects=[p for p in sorted(obj.glob('*.o')) if p.name not in ['sim_nos_sim.c.o','nos_faces_face_orrery.c.o']]
    r.command([r.GCC,*objects,sim,test,'-o',exe,'-lgdi32','-luser32','-lwinmm','-lm'],r.OUT/f'raster_link_{kind}.log')
    r.run(exe,ROOT/'tools/orrery/perf_round7.txt',f'raster_{kind}','--no-media')
    print('PASS raster',kind,flush=True)

def blur_units():
    flags=['-O2','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-format-truncation',
        '-flto','-fwhole-program','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        '-D_assert=nos_test_assert','-D_wassert=nos_test_wassert','-Icore','-Inos','-Inos/fonts']
    modules={'mblur':['nos/mblur.c'],'mblur_gate':[],
        'pblur':['nos/mblur.c','core/sprite.c'],'kblur_slice':[],'psram':['nos/psram.c']}
    # Verify the assertion failure path itself before running any blur program.
    probe=r.OUT/'assert_probe.c';probe.write_text('#include <assert.h>\nint main(void){assert(0);}\n')
    exe=r.OUT/'assert_probe.exe'
    r.command([r.GCC,*flags,probe,'tests/no_dialog.c','-o',exe],r.OUT/'assert_build.log')
    p=subprocess.run([str(exe)],capture_output=True,timeout=10)
    assert p.returncode==2 and b'FAIL' in p.stderr
    for module,sources in modules.items():
        exe=r.OUT/f'{module}.exe'
        r.command([r.GCC,*flags,f'tests/{module}_test.c',*sources,'tests/no_dialog.c',
            '-o',exe,'-lm'],r.OUT/f'{module}_build.log')
        for arg in (['','1','2','3'] if module=='pblur' else ['']):
            r.command([exe,*([arg] if arg else [])],r.OUT/f'{module}{arg}.log')
        print('PASS',module,flush=True)

def memory():
    # Compile only: no firmware link, flash, serial or device access.
    compilers=list((Path.home()/'.espressif/tools/xtensa-esp-elf').rglob('xtensa-esp32s3-elf-gcc.exe'))
    if not compilers:raise RuntimeError('ESP32-S3 compiler required for target size check')
    # Invoke the compiler directly: Espressif's Rust alias launcher needs
    # Windows path privileges unavailable in some restricted workspaces.
    cc=compilers[-1].with_name('xtensa-esp-elf-gcc.exe'); nm=cc.with_name('xtensa-esp-elf-nm.exe')
    config=cc.parent.parent/'lib/xtensa_esp32s3.so'
    sizes={}
    for kind,source in [('before',snapshot()),('after',ROOT)]:
        probe=r.OUT/f'size_{kind}.c';obj=probe.with_suffix('.o')
        types={'state':'orr_state','list':'orr_list','orbit':'orr_orbit_cache','profile':'orr_prof'}
        if kind=='after':types.update(ticks='orr_tick_cache',tick_work='*orr_tick_work',tick_ptrs='orr_ticks',work_ptr='orr_tick_work')
        probe.write_text(f'#include "{(source/"nos/faces/face_orrery.c").as_posix()}"\n'+
            '\n'.join(f'unsigned char measure_{n}[sizeof({t})];' for n,t in types.items())+'\n')
        r.command([cc,f'-mdynconfig={config}','-O2','-std=c99','-Wall','-Wextra','-Wno-unused-parameter',
            '-ffunction-sections','-fdata-sections','-fstack-usage',
            *(f'-I{source/p}' for p in ['core','nos','nos/fonts','sim']),'-c',probe,'-o',obj],r.OUT/f'size_{kind}.log')
        output=r.command([nm,'-S',obj],r.OUT/f'size_{kind}_symbols.log')
        sizes[kind]={line.split()[-1][8:]:int(line.split()[1],16) for line in output.splitlines() if ' measure_' in line}
    b,a=sizes['before'],sizes['after']
    sizes['added_psram']=sum(a[n]-b[n] for n in ['state','list','orbit'])+3*a['ticks']+4096*a['tick_work']
    sizes['added_internal']=a['profile']-b['profile']+a['tick_ptrs']+a['work_ptr']
    (r.OUT/'memory.json').write_text(json.dumps(sizes,indent=2))
    print('TARGET MEMORY',sizes,flush=True)

def validate():
    exes={k:run(k,snapshot() if k=='before' else priced_source() if k=='priced' else ROOT,k!='nocost')
          for k in ['before','priced','after','nocost']}
    unit('after');unit('nocost',False);blur_units();memory()
    hashes={}
    for shot in ['now','travelled','returned','tilt']:
        hashes[shot]={k:hashlib.sha256((r.OUT/k/f'style5_{shot}.png').read_bytes()).hexdigest() for k in exes}
        assert len(set(hashes[shot].values()))==1, (shot,hashes[shot])
    assert (r.OUT/'before.hash').read_bytes()==(r.OUT/'priced.hash').read_bytes()
    assert (r.OUT/'after.hash').read_bytes()==(r.OUT/'nocost.hash').read_bytes()
    scripts=sorted((ROOT/'tools/orrery').rglob('*.txt'))+[ROOT/'tools/nos_blur.txt',ROOT/'tools/nos_blur_pages.txt']
    for script in scripts:
        name='check_'+'_'.join(script.relative_to(ROOT/'tools').with_suffix('').parts)
        r.run(exes['after'],script,name,'--no-media')
        print('PASS',script.relative_to(ROOT),flush=True)
    overlap={}
    for kind in ['before','priced','after']:
        dest=r.run(exes[kind],ROOT/'tools/orrery/month_scrub.txt',kind+'_month','--no-media')
        overlap[kind]=summary(dest)
    damage=r.OUT/'damage.txt'
    damage.write_text('\n'.join(line for line in (ROOT/'tools/orrery/arc_motion.txt').read_text().splitlines()
        if not line.startswith('expect '))+'\n')
    r.command([exes['after'],'--damagecheck',damage,'--out',r.OUT/'damage'],r.OUT/'damage.log')
    result={'timing':{k:summary(r.OUT/k) for k in exes},'grip_overlap':overlap,'settled_png_sha256':hashes,
        'scripts':len(scripts),'raster_modes':['cost','nocost'],'damage':'PASS','assertion_failure_exit':2}
    (r.OUT/'results.json').write_text(json.dumps(result,indent=2))
    print('PASS settled HEAD PNGs, cost/no-cost hashes, damage/full, all scripts and native tests',flush=True)

if __name__ == '__main__':
    import argparse
    p=argparse.ArgumentParser()
    p.add_argument('kind', choices=['before','priced','after','nocost','validate','memory'])
    a=p.parse_args()
    if a.kind=='validate':validate()
    elif a.kind=='memory':memory()
    else:run(a.kind, snapshot() if a.kind=='before' else priced_source() if a.kind=='priced' else ROOT, a.kind!='nocost')
