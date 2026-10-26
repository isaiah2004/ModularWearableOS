"""Build/capture ringfix from source, with 6b4efd0 as the isolated baseline."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import ctypes, csv, importlib.util, io, json, os, statistics, subprocess, tarfile
from PIL import Image, ImageDraw, ImageFont
ROOT=Path(__file__).resolve().parents[3]
os.chdir(ROOT)
OUT=ROOT/'build/orrery/ringfix'
OUT.mkdir(parents=True,exist_ok=True)
if os.name=='nt': ctypes.windll.kernel32.SetErrorMode(0x0001|0x0002|0x8000)
GCC=Path('C:/mingw64/bin/gcc.exe')

def command(args,log):
    p=subprocess.run([str(a) for a in args],capture_output=True,text=True)
    log.parent.mkdir(parents=True,exist_ok=True)
    log.write_text(p.stdout+p.stderr,encoding='utf-8')
    if p.returncode: raise RuntimeError(f'{args[0]} exit {p.returncode}: {log}')
    return p.stdout

def build(kind,source=ROOT,cost=True):
    core='gfx font sprite anim notif battery pedometer walk motion_wake wake_service knock ephem'.split()
    sources=[*sorted((source/'nos').rglob('*.c')),*(source/'core'/f'{n}.c' for n in core),
             *sorted((source/'core/fonts').glob('*.c')),*sorted((source/'sim').glob('nos_*.c'))]
    obj=OUT/f'obj_{kind}';obj.mkdir(exist_ok=True)
    flags=['-O2','-std=c99','-DNOS_PSRAM_TRACKING','-D_WIN32_WINNT=0x0600',*(f'-I{source/p}' for p in ['core','nos','nos/fonts','sim'])]
    if cost: flags+=['-DGFX_COST_MODEL']
    newest=max(p.stat().st_mtime for folder in ['core','nos','sim'] for p in (source/folder).rglob('*.h'))
    def one(src):
        target=obj/('_'.join(src.relative_to(source).parts)+'.o')
        if not target.exists() or target.stat().st_mtime<max(newest,src.stat().st_mtime):
            command([GCC,*flags,'-c',src,'-o',target],target.with_suffix('.log'))
        return target
    with ThreadPoolExecutor(max_workers=8) as pool: objects=list(pool.map(one,sources))
    exe=OUT/f'{kind}.exe'
    command([GCC,*objects,'-o',exe,'-lgdi32','-luser32','-lwinmm','-lm'],OUT/f'build_{kind}.log')
    print('BUILT',kind,flush=True)
    return exe

def baseline():
    dest=OUT/'baseline-source'
    if not (dest/'nos/faces/face_orrery.c').exists():
        data=subprocess.check_output(['git','archive','6b4efd0','core','nos','sim','firmware/main'],stderr=subprocess.PIPE)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            for member in archive.getmembers():
                if not member.isfile() or Path(member.name).suffix not in ('.c','.h','.inc'):continue
                p=(dest/member.name).resolve(); assert p.is_relative_to(dest.resolve())
                p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(archive.extractfile(member).read())
    return build('before',dest)

def run(exe,script,name,*extra):
    dest=OUT/name
    command([exe,'--face','8','--script',script,'--out',dest,'--quiet',*extra],OUT/f'{name}.log')
    return dest

def summary(dest):
    rows=list(csv.DictReader((dest/'frames.csv').open()))
    result={}
    for section in dict.fromkeys(r['section'] for r in rows):
        group=[r for r in rows if r['section']==section]; times=sorted(float(r['est_frame_ms']) for r in group)
        result[section]=dict(frames=len(group),late=sum(int(r['late']) for r in group),
            p50_ms=statistics.median(times),p95_ms=times[int(.95*(len(times)-1))],max_ms=max(times))
    return result

def images():
    font=ImageFont.truetype('C:/Windows/Fonts/consola.ttf',22)
    sheet=Image.new('RGB',(1964,1132),'#202124');d=ImageDraw.Draw(sheet)
    for row,variant in enumerate(['A','B']):
        d.text((20,row*556+10),f'VARIANT {variant}',font=font,fill='white')
        for col,(shot,label) in enumerate([('now','NOW'),('travelled','TRAVELLED +99 D'),('drag','MID-DRAG'),('tilt','3D TILT')]):
            x,y=20+col*486,50+row*556
            im=Image.open(OUT/variant/f'style5_{shot}.png').convert('RGB')
            sheet.paste(im,(x,y));d.text((x,y+472),label,font=font,fill='white')
            im.crop((100,350,366,450)).resize((532,200),Image.Resampling.NEAREST).save(OUT/f'plate_{variant}_{shot}_2x.png')
    sheet.save(OUT/'sheet.png')
    plates=Image.new('RGB',(1084,500),'#202124');pd=ImageDraw.Draw(plates)
    for row,v in enumerate(['A','B']):
        for col,shot in enumerate(['now','travelled']):
            x,y=10+col*542,10+row*250
            pd.text((x,y),f'{v} / {shot.upper()}',font=font,fill='white')
            plates.paste(Image.open(OUT/f'plate_{v}_{shot}_2x.png'),(x,y+34))
    plates.save(OUT/'plates_2x.png')
    for name,folder in [('before','before'),('after','A')]:
        for shot in ['now','travelled','drag']:
            im=Image.open(OUT/folder/f'style5_{shot}.png')
            im.crop((135,0,330,95)).resize((390,190),Image.Resampling.NEAREST).save(OUT/f'highlights_{name}_{shot}_2x.png')

def highlight_sheet():
    font=ImageFont.truetype('C:/Windows/Fonts/consola.ttf',20)
    im=Image.new('RGB',(810,240),'#202124');d=ImageDraw.Draw(im)
    for col,(kind,label) in enumerate([('before','BEFORE 6b4efd0'),('after','AFTER')]):
        x=10+col*400;d.text((x,10),label,font=font,fill='white')
        im.paste(Image.open(OUT/f'highlights_{kind}_now_2x.png'),(x,40))
    im.save(OUT/'highlights_2x.png')

def resting(exe,name,variant=None):
    script=OUT/f'{name}_rest.txt'
    switch=f'ringplate {variant}\n' if variant else ''
    script.write_text('face 8\nringstyle 5\n'+switch+'date 2026-10-11\ntime 10:08:15\n'
        'wait 2500\nhold 300 300 1700\nwait 700\ntap 380 75\nwait 1200\n'
        'mark rings_rest\nwait 1000\nexpect idle\nexpect nolate\n')
    return summary(run(exe,script,name+'_rest','--no-media'))['rings_rest']

def main():
    before=baseline(); final=build('cost'); nocost=build('nocost',cost=False)
    original=(ROOT/'tools/orrery/ringread/style5.txt').read_text()
    run(before,ROOT/'tools/orrery/ringread/style5.txt','before')
    scripts={}
    for variant in ['A','B']:
        script=OUT/f'variant_{variant}.txt'
        content=original.replace('ringstyle 5',f'ringstyle 5\nringplate {variant}')
        if variant=='B':content=content.replace('tap 283 436','tap 171 428')
        script.write_text(content);scripts[variant]=script
        run(final,script,variant)
    images()
    highlight_sheet()
    costs={'before':summary(OUT/'before'),**{v:summary(OUT/v) for v in ['A','B']}}
    for name,exe,variant in [('before',before,None),('A',final,'A'),('B',final,'B')]:
        costs[name]['rings_rest']=resting(exe,name,variant)
    for variant in ['A','B']:
        for section in costs['before']:
            assert costs[variant][section]['late']<=costs['before'][section]['late'], (variant,section,'late regression')
        # Same-minute read-out after tapping NOW must match its initial state.
        a=Image.open(OUT/variant/'style5_now.png').crop((108,358,358,445))
        b=Image.open(OUT/variant/'style5_returned.png').crop((108,358,358,445))
        assert a.tobytes()==b.tobytes(), (variant,'return did not restore read-out')
    (OUT/'costs.json').write_text(json.dumps(costs,indent=2)+'\n')
    print(json.dumps(costs,indent=2),flush=True)
    all_scripts=sorted((ROOT/'tools/orrery').rglob('*.txt'))
    for script in all_scripts:
        name='check_'+'_'.join(script.relative_to(ROOT/'tools/orrery').with_suffix('').parts)
        run(final,script,name,'--no-media');print('PASS',name,flush=True)
    run(final,ROOT/'tools/orrery/ringfix/switch.txt','switch_images')
    switch_dir=OUT/'switch_images'
    a=Image.open(switch_dir/'switch_A.png');b=Image.open(switch_dir/'switch_A_again.png')
    assert a.tobytes()==b.tobytes(), 'switch did not invalidate retained plate commands'
    a=Image.open(switch_dir/'switch_A_returned.png');b=Image.open(switch_dir/'switch_B_returned.png')
    assert a.crop((108,358,358,445)).tobytes()==b.crop((108,358,358,445)).tobytes(), 'negative return mismatch'
    for v,script in scripts.items():
        px=OUT/f'pixels_{v}.txt'
        px.write_text('\n'.join(l for l in script.read_text().splitlines() if l not in ('expect idle','expect nolate'))+'\n')
        for kind,exe,args in [('cost',final,[]),('nocost',nocost,[]),('full',final,['--force-full'])]:
            run(exe,px,f'pixels_{v}_{kind}','--hashes',OUT/f'{v}_{kind}.hash','--no-media',*args)
        assert (OUT/f'{v}_cost.hash').read_bytes()==(OUT/f'{v}_nocost.hash').read_bytes()
        assert (OUT/f'{v}_cost.hash').read_bytes()==(OUT/f'{v}_full.hash').read_bytes()
    for kind,exe in [('before',before),('after',final)]:
        run(exe,ROOT/'tools/orrery/regress.txt',f'{kind}_locked')
    for name in ['r1_aod','r2_wake_locked']:
        assert Image.open(OUT/'before_locked'/f'{name}.png').tobytes()==Image.open(OUT/'after_locked'/f'{name}.png').tobytes()
    (OUT/'validation.json').write_text(json.dumps(dict(baseline='6b4efd0',scripts=len(all_scripts),all_scripts_exit_zero=True,late_counts_no_worse=True,
        both_variants_cost_nocost_exact=True,both_variants_damage_full_exact=True,locked_aod_exact=True,both_now_returns_exact=True,runtime_switch_exact=True),indent=2)+'\n')
    print('PASS review, all scripts, both variants cost/no-cost and damage/full hashes, locked/AOD',flush=True)
if __name__=='__main__':main()
