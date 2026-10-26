"""Round-7 calibrated round-6 baseline and independent iris clipping oracle.
Run check_round7.ps1 first. All outputs are local; no device access.
Baseline changes only cost instrumentation and the observational profiler.
"""
from pathlib import Path
import subprocess, shutil, csv, statistics, os
root=Path(__file__).resolve().parents[2];os.chdir(root);out=root/'build/orrery/round7';out.mkdir(parents=True,exist_ok=True)
gcc=shutil.which('gcc') or 'C:/mingw64/bin/gcc.exe'; flags=['-O2','-std=c99','-DGFX_COST_MODEL','-Icore','-Inos','-Inos/fonts','-Isim']
base=out/'baseline';base.mkdir(exist_ok=True)
for n in ('face_orrery.c','face_orrery_draw.h','face_orrery_runs.h','face_orrery_controls.h','face_orrery_profile.h'):
    (base/n).write_bytes(subprocess.check_output(['git','show','f5113b4:nos/faces/'+n]))
s=(base/'face_orrery_draw.h').read_text().replace('GFX_COST_PX(GFX_COST_ORR_CPU,8*(x1-x0)*part.h+120*lit);','GFX_COST_PX(GFX_COST_ORR_POSTDIM,(x1-x0)*part.h);\n                GFX_COST_PX(GFX_COST_ORR_CPU,120*lit);')
(base/'face_orrery_draw.h').write_text(s)
# Current profiler is observational and allows model-only peak attribution in baseline.
shutil.copyfile(root/'nos/faces/face_orrery_profile.h',base/'face_orrery_profile.h')
s=(base/'face_orrery.c').read_text(encoding='utf-8');a=s.index('#ifndef GFX_COST_MODEL\nextern uint32_t nos_dbg_us');b=s.index('\nstatic void tick',a)
new=(root/'nos/faces/face_orrery.c').read_text(encoding='utf-8');na=new.index('/* Tick timings belong');nb=new.index('\nstatic void tick',na)
s=s[:a]+new[na:nb]+s[b:];s=s.replace('    scene_update(c);\n    S.jd_prev_frame','    scene_update(c);\n    orr_prof_context(c);\n    S.jd_prev_frame');(base/'face_orrery.c').write_text(s,encoding='utf-8')
(base/'nos.c').write_bytes(subprocess.check_output(['git','show','f5113b4:nos/nos.c']))
for n in ('face_orrery','nos'):
    subprocess.run([gcc,*flags,'-c',str(base/f'{n}.c'),'-o',str(base/f'{n}.o')],check=True)
objects=[str(p) for p in (root/'build/nos_obj').glob('*.o') if p.name not in ('nos_faces_face_orrery.o','nos_nos.o')]
for mode,face in [('baseline',base/'face_orrery.o'),('ink_only',root/'build/nos_obj/nos_faces_face_orrery.o')]:
    exe=out/f'{mode}.exe'
    subprocess.run([gcc,*objects,str(face),str(base/'nos.o'),'-o',str(exe),'-lgdi32','-luser32','-lwinmm','-lm'],check=True)
script=out/'perf.txt';script.write_text('\n'.join(l for l in (root/'tools/orrery/perf_round7.txt').read_text().splitlines() if not l.startswith('expect '))+'\n')
for mode,exe in [('baseline',out/'baseline.exe'),('ink_only',out/'ink_only.exe'),('fixed',root/'build/nos_sim_round7.exe')]:
    with (out/f'{mode}.log').open('w') as log:
        subprocess.run([str(exe),'--script',str(script),'--out',str(out/mode),'--hashes',str(out/f'{mode}.hash'),'--no-media','--quiet'],stdout=log,stderr=log,check=True)
    rows=list(csv.DictReader((out/mode/'frames.csv').open()))
    for sec in ['settings_open1','settings_open2','notifs_open','notifs_close','pan','pan_fling','wake1','wake2']:
        r=[r for r in rows if r['section']==sec and int(r['dirty_px'])]
        v=[float(r['est_frame_ms']) for r in r]
        print(mode,sec,'p50',statistics.median(v),'max',max(v),'late',sum(int(r['late']) for r in r),'/',len(v),flush=True)
assert (out/'ink_only.hash').read_bytes()==(out/'fixed.hash').read_bytes(),'engine clip mismatch'
print('PASS clipping equivalence',flush=True)

# Engine-only changes must be exact for every face, origin, blur and reversal.
sweep=out/'iris_sweep.txt'
lines=[]
for face in range(9):
    lines += ['home_now',f'face {face}','wait 2500']
    for blur in (0,1):
        for x,y in ((233,233),(380,100),(0,233)):
            lines += [f'set iris_blur {blur}',f'mark face{face}_blur{blur}',
                      f'open settings {x} {y}','wait 600','pop','wait 600',
                      f'open notifs {x} {y}','wait 100','pop','wait 600']
sweep.write_text('\n'.join(lines)+'\n')
total=0
for source in [*sorted((root/'tools/orrery').glob('*.txt')),sweep]:
    script=out/('check_'+source.name)
    script.write_text('\n'.join(l for l in source.read_text().splitlines()
                                if not l.lstrip().startswith('expect '))+'\n')
    hashes=[]
    for mode,exe in [('ink',out/'ink_only.exe'),('damage',root/'build/nos_sim_round7.exe'),
                     ('full',root/'build/nos_sim_round7.exe')]:
        dest=out/(source.stem+'_'+mode);digest=out/(source.stem+'_'+mode+'.hash')
        args=[str(exe),'--script',str(script),'--out',str(dest),'--hashes',str(digest),'--no-media','--quiet']
        if mode=='full':args+=['--force-full']
        with dest.with_suffix('.log').open('w') as log:
            subprocess.run(args,stdout=log,stderr=log,check=True)
        hashes.append(digest.read_bytes())
    assert hashes[0]==hashes[1],f'engine clipping pixels: {source.name}'
    assert hashes[1]==hashes[2],f'damage/full pixels: {source.name}'
    n=len(hashes[0].splitlines());total+=n
    print(f'PASS engine-only and damage/full {source.name}: {n} frames',flush=True)
print(f'PASS {total} frames per comparison',flush=True)
# Quantify the intended ink-before-AA rounding difference on a mid-iris frame.
for mode,exe in [('old',out/'baseline.exe'),('new',root/'build/nos_sim_round7.exe')]:
    subprocess.run([str(exe),'--script',str(out/'perf.txt'),'--out',str(out/('pixels_'+mode)),
                    '--dump-frame','157','--no-media','--quiet'],check=True,stdout=subprocess.DEVNULL)
from array import array
a=array('H',(out/'pixels_old/frame_157.bin').read_bytes())
b=array('H',(out/'pixels_new/frame_157.bin').read_bytes())
diffs=[(x,y) for x,y in zip(a,b) if x!=y]
maximum=[max(abs(((x>>shift)&mask)-((y>>shift)&mask)) for x,y in diffs) for shift,mask in [(11,31),(5,63),(0,31)]]
print('Mid-iris ink/AA rounding:',len(diffs),'pixels differ; max RGB565 lane delta',maximum,flush=True)
