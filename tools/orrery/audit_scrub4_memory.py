"""ESP32-S3 writable-symbol audit; compile only, never link/flash/open a port."""
from pathlib import Path
import io, json, subprocess, tarfile
import check_scrub4 as c

def main():
    sources={'round3':c.snapshot(), 'after':c.ROOT}
    dest=c.OUT/'round2'
    if not (dest/'nos/faces/face_orrery.c').exists():
        data=subprocess.check_output(['git','archive','a684695','core','nos','sim'],stderr=subprocess.PIPE)
        with tarfile.open(fileobj=io.BytesIO(data)) as ar:
            for member in ar.getmembers():
                if not member.isfile():continue
                p=(dest/member.name).resolve();assert p.is_relative_to(dest.resolve())
                p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(ar.extractfile(member).read())
    sources['round2']=dest
    cc=next(Path('C:/Users/isaia/.espressif/tools/xtensa-esp-elf').rglob('xtensa-esp-elf-gcc.exe'))
    config=cc.parent.parent/'lib/xtensa_esp32s3.so'
    result={}
    for kind,source in sources.items():
        symbols={}
        for file in ['nos/faces/face_orrery.c','nos/ui_arc.c','core/ephem.c','nos/nos.c']:
            stem=kind+'_'+Path(file).stem
            obj=c.OUT/f'memory_{stem}.o'
            c.b.command([cc,f'-mdynconfig={config}','-O2','-std=c99','-DNOS_PSRAM_TRACKING',
                '-ffunction-sections','-fdata-sections','-fstack-usage',
                *(f'-I{source/p}' for p in ['core','nos','nos/fonts','sim']),
                '-c',source/file,'-o',obj],c.OUT/f'memory_{stem}.log')
            output=c.b.command([cc.with_name('xtensa-esp-elf-nm.exe'),'-S',obj],c.OUT/f'memory_{stem}_symbols.log')
            symbols[file]={parts[3]:int(parts[1],16) for line in output.splitlines()
                           if len(parts:=line.split())==4 and parts[2] in 'bBdD'}
        result[kind]={'bytes':sum(sum(s.values()) for s in symbols.values()),'symbols':symbols}
    result['round3_growth']=result['round3']['bytes']-result['round2']['bytes']
    result['reclaimed']=result['round3']['bytes']-result['after']['bytes']
    result['projected_internal_free']=16099+result['reclaimed']
    assert result['projected_internal_free']>=24*1024
    (c.OUT/'internal_memory.json').write_text(json.dumps(result,indent=2))
    print(json.dumps({k:v if not isinstance(v,dict) else v['bytes'] for k,v in result.items()},indent=2))

if __name__=='__main__':main()
