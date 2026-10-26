"""77ac23a board-calibrated scrub replay. Read-only Git, no device I/O.
Build current model/no-cost executables with check_round7.ps1 first.
Artifacts: build/orrery/scrub2_check. Child processes inherit no-error-box mode.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse, csv, ctypes, importlib.util, io, json, os, statistics, subprocess, tarfile
ROOT=Path(__file__).resolve().parents[2]
os.chdir(ROOT)
OUT=ROOT/'build/orrery/scrub2_check'
OUT.mkdir(parents=True,exist_ok=True)
CC=Path('C:/mingw64/bin/gcc.exe')
if os.name=='nt': ctypes.windll.kernel32.SetErrorMode(0x8003)
FLAGS=['-O2','-std=c99','-DGFX_COST_MODEL','-DNOS_PSRAM_TRACKING']
LIBS=['-lgdi32','-luser32','-lwinmm','-lm']
def run(args,log):
    p=subprocess.run([str(x) for x in args],capture_output=True,timeout=240)
    log.write_bytes(p.stdout+p.stderr)
    if p.returncode: raise RuntimeError(f'exit {p.returncode}: {log}')
    return p.stdout

def baseline():
    dest=OUT/'baseline'
    data=subprocess.check_output(['git','archive','77ac23a','core','nos','sim','firmware/main'],stderr=subprocess.PIPE)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for m in archive.getmembers():
            if not m.isfile() or Path(m.name).suffix not in ('.c','.h','.inc'):continue
            p=(dest/m.name).resolve()
            assert p.is_relative_to(dest.resolve())
            p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(archive.extractfile(m).read())
    p=dest/'nos/faces/face_orrery.c'
    s=p.read_text().replace('S.nbelt*3000','S.nbelt*9700').replace('S.nbelt*500','S.nbelt*2550')
    p.write_text(s)
    p=dest/'nos/faces/face_orrery_draw.h'
    s=p.read_text().replace('GFX_COST_PX(GFX_COST_ORR_BUILD,(S.nbelt+S.nsm+S.ncm)*5/8+l->ndots*225/1000);',
        'GFX_COST_PX(GFX_COST_ORR_PRE_CPU,950*(S.nbelt+S.nsm+S.ncm)+80*(S.nbelt+S.nsm+S.ncm)+80*ORR_ORBIT_ROWS);')
    p.write_text(s)
    core='gfx font sprite anim notif battery pedometer walk motion_wake wake_service knock ephem'.split()
    sources=[*sorted((dest/'nos').rglob('*.c')),*(dest/'core'/f'{n}.c' for n in core),
             *sorted((dest/'core/fonts').glob('*.c')),*sorted((dest/'sim').glob('nos_*.c'))]
    inc=[f'-I{dest/p}' for p in ['core','nos','nos/fonts','sim']]
    def compile_one(src):
        obj=src.with_suffix('.o');run([CC,*FLAGS,*inc,'-c',src,'-o',obj],src.with_suffix('.log'));return obj
    with ThreadPoolExecutor(max_workers=8) as pool:objects=list(pool.map(compile_one,sources))
    run([CC,*objects,'-o',OUT/'before.exe',*LIBS],OUT/'baseline-link.log')
    print('Built 77ac23a with cost-only board calibration',flush=True)

def compare():
    exes={'before':OUT/'before.exe','after':ROOT/'build/nos_sim_round7.exe','nocost':ROOT/'build/nos_sim_round7_nocost.exe'}
    results={}
    for script in ['month_scrub','scrub_board','arc_motion']:
        results[script]={}
        for name,exe in exes.items():
            dest=OUT/f'{script}_{name}'
            run([exe,'--script',f'tools/orrery/{script}.txt','--out',dest,'--hashes',dest.with_suffix('.hash'),'--quiet'],dest.with_suffix('.log'))
            rows=list(csv.DictReader((dest/'frames.csv').open()))
            values={}
            for sec in dict.fromkeys(r['section'] for r in rows):
                active=[r for r in rows if r['section']==sec and int(r['dirty_px'])]
                if not active:continue
                work=[float(r['est_raster_ms']) for r in active]
                frames=[float(r['est_frame_ms']) for r in active]
                values[sec]={'sent':len(active),'work_mean':statistics.mean(work),'work_max':max(work),
                    'frame_median':statistics.median(frames),'frame_max':max(frames),
                    'controls_max':max(float(r['orr_0_controls']) for r in active)}
            results[script][name]=values
        assert (OUT/f'{script}_after.hash').read_bytes()==(OUT/f'{script}_nocost.hash').read_bytes(),f'{script} hooks changed pixels'
        # Settled hashes: both regression scripts finish with an idle interval.
        before=(OUT/f'{script}_before.hash').read_text().splitlines()[-1].split()[1]
        after=(OUT/f'{script}_after.hash').read_text().splitlines()[-1].split()[1]
        assert before==after,f'{script} settled final differs'
        for png in (OUT/f'{script}_before').glob('*.png'):
            if png.stem not in ('scrub_settled','style5_now','style5_travelled','style5_returned','style5_tilt'):continue
            assert png.read_bytes()==(OUT/f'{script}_after'/png.name).read_bytes(),f'settled PNG differs: {png}'
        print(f'PASS {script}: settled 77ac23a pixels and cost/no-cost frame hashes',flush=True)
    # Full repaint vs dirty strips must agree after the animation previews, too.
    damage=OUT/'damage.txt'
    damage.write_text('\n'.join(x for x in Path('tools/orrery/scrub_board.txt').read_text().splitlines() if not x.startswith('expect '))+'\n')
    run([exes['after'],'--damagecheck',damage,'--out',OUT/'damage'],OUT/'damage.log')
    for script,sections in [('month_scrub',['month_scrub','settle']),('scrub_board',['scrub_forward','scrub_back','settle'])]:
        for sec in sections:
            assert results[script]['after'][sec]['work_max']<=33.0,(script,sec,results[script]['after'][sec])
    (OUT/'results.json').write_text(json.dumps(results,indent=2))
    print('PASS modeled CPU work <=33 ms for both swipe directions, stress scrub and settle; damage check',flush=True)
    print(json.dumps({s:{n:{k:v for k,v in a.items() if 'scrub' in k} for n,a in data.items() if n!='nocost'} for s,data in results.items() if s!='arc_motion'},indent=2))

def support_checks():
    spec=importlib.util.spec_from_file_location('scrub_checks',ROOT/'tools/orrery/check_scrub.py')
    old=importlib.util.module_from_spec(spec);spec.loader.exec_module(old)
    old.r.OUT=OUT/'support';old.r.OUT.mkdir(parents=True,exist_ok=True)
    old.blur_units()  # includes an intentional assertion: stderr + exit 2, no dialog
    spec=importlib.util.spec_from_file_location('arc_checks',ROOT/'tools/orrery/check_arc_motion.py')
    arc=importlib.util.module_from_spec(spec);spec.loader.exec_module(arc)
    if not (arc.OUT/'legacy.o').exists():arc.baseline()
    arc.unit()
    compilers=list((Path.home()/'.espressif/tools/xtensa-esp-elf').rglob('xtensa-esp32s3-elf-gcc.exe'))
    if not compilers:raise RuntimeError('ESP32-S3 compiler required for target size check')
    cc=compilers[-1].with_name('xtensa-esp-elf-gcc.exe');nm=cc.with_name('xtensa-esp-elf-nm.exe')
    config=cc.parent.parent/'lib/xtensa_esp32s3.so'
    sizes={}
    for kind,source in [('before',OUT/'baseline'),('after',ROOT)]:
        probe=OUT/f'size_{kind}.c';obj=probe.with_suffix('.o')
        types={'state':'orr_state','list':'orr_list','orbit':'orr_orbit_cache','profile':'orr_prof',
               'ast':'ast_k','ticks':'orr_tick_cache','tick_work':'*orr_tick_work',
               'tick_ptrs':'orr_ticks','work_ptr':'orr_tick_work'}
        if kind=='after':types['queue']='belt_due'
        else:types['hot']='orr_hot'
        probe.write_text(f'#include "{(source/"nos/faces/face_orrery.c").as_posix()}"\n'+
            '\n'.join(f'unsigned char measure_{n}[sizeof({t})];' for n,t in types.items())+'\n')
        run([cc,f'-mdynconfig={config}','-O2','-std=c99','-Wall','-Wextra','-Wno-unused-parameter',
             '-ffunction-sections','-fdata-sections','-fstack-usage',
             *(f'-I{source/p}' for p in ['core','nos','nos/fonts','sim']),'-c',probe,'-o',obj],OUT/f'size_{kind}.log')
        output=run([nm,'-S',obj],OUT/f'size_{kind}_symbols.log').decode()
        sizes[kind]={line.split()[-1][8:]:int(line.split()[1],16) for line in output.splitlines() if ' measure_' in line}
    sizes['after']['hot']=0 # duplicate internal reference array removed in scrub 4
    b,a=sizes['before'],sizes['after']
    sizes['added_psram']=sum(a[n]-b[n] for n in ['state','list','orbit'])+3999*(a['ast']-b['ast']+a['queue'])+3*(a['ticks']-b['ticks'])+8192*a['tick_work']-4096*b['tick_work']
    sizes['added_internal']=sum((0 if n=='profile' else a[n])-b[n] for n in ['profile','hot','tick_ptrs','work_ptr'])
    (OUT/'memory.json').write_text(json.dumps(sizes,indent=2))
    print('PASS ESP32-S3 compile and sizes:',sizes,flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--skip-baseline',action='store_true');args=parser.parse_args()
    if not args.skip_baseline:baseline()
    compare()
    support_checks()
