"""Export canonical native menu artwork and pack it for the SD card; --verify checks the tracked packs.

Packs (format in docs/SD_ASSETS.md) replace the former giant C arrays:
  assets/sd/menu/native_menu_sharp.pack  level 0 only: what the SHARP device build draws
  assets/sd/menu/native_menu_full.pack   all blur levels: simulator and host tests
"""
from pathlib import Path
import csv,hashlib,json,struct,sys,zlib
REPO=Path(__file__).resolve().parents[1]
ROOT=REPO/'build/native_menu_asset_export'
PACK_DIR=REPO/'assets/sd/menu'
STAGE_DIR=REPO/'build/sd/menu'
MANIFEST=REPO/'nos/native_menu_assets_manifest.inc'
ID_HEADER=REPO/'nos/native_menu_assets_id.h'
PACK_FILES={'sharp':'native_menu_sharp.pack','full':'native_menu_full.pack'}
PACK_MAGIC=b'NMAP'
PACK_VERSION=1
PACK_FLAG_SHARP=1
PACK_HEADER=struct.Struct('<4sHH'+'I'*19+'32s32sI')
RECORD=struct.Struct('<4B3I4h')
BODY=struct.Struct('<6I')
PAL_ORDER=['settings_neutral.lut','settings_red_0.lut','settings_red_1.lut','launcher_neutral.lut','launcher_press.lut']
PAL_BYTES=33280*4+512
CURVE_N=650
NONE=65535
FAMILIES={'settings':0,'icon':1,'name':2}

def sha(b):return hashlib.sha256(b).hexdigest()
def fnv1a(raw):
 h=2166136261
 for v in raw:h=((h^v)*16777619)&0xffffffff
 return h
def pack_image(raw,width):
 """Best of raw, plain deflate or previous-row-delta deflate; (data,stride,compressed?)."""
 assert 0<width<=65535 and len(raw)>=width and len(raw)%width==0
 plain=zlib.compress(raw,9)
 delta=raw[:width]+bytes((raw[i]-raw[i-width])&255 for i in range(width,len(raw)))
 predicted=zlib.compress(delta,9)
 packed,stride=(predicted,width) if len(predicted)<len(plain) else (plain,0)
 if len(packed)>=len(raw):return raw,0,0
 decoded=bytearray(zlib.decompress(packed))
 if stride:
  for i in range(stride,len(decoded)):decoded[i]=(decoded[i]+decoded[i-stride])&255
 assert decoded==raw
 return packed,stride,len(packed)
def pad4(n):return (n+3)&~3
def meta_layout(n_records,n_apps,reg_str_bytes):
 """Offsets inside the meta section; nos/native_menu_assets.c computes the same."""
 L={'rec':0};L['idx_s']=n_records*24;L['idx_i']=L['idx_s']+36*2*9*2;L['idx_n']=L['idx_i']+n_apps*3*9*2
 L['curve']=L['idx_n']+n_apps*9*2;L['reg_icon']=L['curve']+CURVE_N*2;L['reg_name']=L['reg_icon']+n_apps*2
 L['str']=L['reg_name']+n_apps*2;L['bytes']=pad4(L['str']+reg_str_bytes)
 return L

def build_pack(m,blob,palettes,curve,sharp,encoded=None):
 """Deterministic pack bytes. m is the manifest; level-0 bodies form one leading section."""
 assets=[a for a in m['assets'] if not sharp or a['level']==0]
 uniq={}
 for a in assets:uniq.setdefault(a['offset'],a)
 l0={a['offset'] for a in assets if a['level']==0}
 order=sorted(l0)+sorted(set(uniq)-l0)
 encoded=encoded if encoded is not None else {}
 arena={};btab=[];stored=bytearray();pos=0
 for off in order:
  a=uniq[off];raw=bytes(blob[off:off+a['bytes']]);assert len(raw)==a['w']*a['h']==a['bytes'] and sha(raw)==a['sha256']
  if off not in encoded:encoded[off]=pack_image(raw,a['w'])+(fnv1a(raw),)
  data,stride,clen,h=encoded[off]
  arena[off]=pos;btab.append((len(stored),pos,len(raw),clen,h,stride));stored+=data;pos+=len(raw)
 l0_n=len(l0);l0_arena=sum(uniq[o]['bytes'] for o in sorted(l0));l0_stored=btab[l0_n][0] if l0_n<len(btab) else len(stored)
 stored+=b'\0'*(pad4(len(stored))-len(stored))
 apps=len(m['launcher_registry']);reg=m['launcher_registry']
 assert [r[0] for r in reg]==list(range(apps))
 strings=bytearray();name_off=[]
 for r in reg:name_off.append(len(strings));strings+=r[2].encode('ascii')+b'\0'
 idx_s=[[[NONE]*9 for _ in range(2)] for _ in range(36)];idx_i=[[[NONE]*9 for _ in range(3)] for _ in range(apps)];idx_n=[[NONE]*9 for _ in range(apps)]
 recs=bytearray()
 for i,a in enumerate(assets):
  f=FAMILIES[a['family']];e,l,v=a['entry'],a['layout'],a['level']
  if f==0:idx_s[e][l][v]=i
  elif f==1:idx_i[e][l][v]=i
  else:idx_n[e][v]=i
  recs+=RECORD.pack(f,e,l,v,arena[a['offset']],a['bytes'],a['revision'],a['w'],a['h'],a['x'],a['y'])
 flat=lambda t:[x for part in t for x in (flat(part) if isinstance(part,list) else [part])]
 L=meta_layout(len(assets),apps,len(strings))
 meta=bytearray(recs)
 meta+=struct.pack('<%dH'%(36*18),*flat(idx_s))+struct.pack('<%dH'%(apps*27),*flat(idx_i))+struct.pack('<%dH'%(apps*9),*flat(idx_n))
 assert len(curve)==CURVE_N*2;meta+=curve
 meta+=struct.pack('<%dh'%apps,*[r[1] for r in reg])+struct.pack('<%dH'%apps,*name_off)+strings
 meta+=b'\0'*(L['bytes']-len(meta));assert len(meta)==L['bytes']
 pal=b''.join(palettes[n] for n in PAL_ORDER);assert len(pal)==PAL_BYTES
 pal_z=zlib.compress(pal,9);assert zlib.decompress(pal_z)==pal
 table=b''.join(BODY.pack(*b) for b in btab)
 meta_off=PACK_HEADER.size;pal_off=meta_off+len(meta);btab_off=pad4(pal_off+len(pal_z));body_off=btab_off+len(table)
 file_bytes=body_off+len(stored)
 payload=bytes(meta)+pal_z+b'\0'*(btab_off-pal_off-len(pal_z))+table+bytes(stored)
 fields=(PACK_HEADER.size,file_bytes,zlib.crc32(payload),len(assets),len(btab),apps,len(strings),meta_off,len(meta),pal_off,len(pal_z),fnv1a(pal),
         btab_off,body_off,len(stored),pos,l0_n,l0_arena,l0_stored)
 head=PACK_HEADER.pack(PACK_MAGIC,PACK_VERSION,PACK_FLAG_SHARP if sharp else 0,*fields,bytes.fromhex(m['build_input_sha256']),bytes.fromhex(m['blob_sha256']),0)
 head=head[:-4]+struct.pack('<I',zlib.crc32(head[:-4]))
 out=head+payload;assert len(out)==file_bytes
 return out

def parse_pack(data,check_fnv=True):
 """Strictly validate a pack and return its decoded content (also the reference reader)."""
 assert len(data)>=PACK_HEADER.size,'short pack'
 t=PACK_HEADER.unpack_from(data)
 magic,ver,flags=t[:3];f=t[3:22];build,blob,hcrc=t[22:]
 (hdr_bytes,file_bytes,pcrc,n_rec,n_body,apps,rs,meta_off,meta_bytes,pal_off,pal_clen,pal_fnv,btab_off,body_off,body_bytes,arena_bytes,
  l0_n,l0_arena,l0_stored)=f
 assert magic==PACK_MAGIC and ver==PACK_VERSION and hdr_bytes==PACK_HEADER.size,'bad pack header'
 assert hcrc==zlib.crc32(data[:hdr_bytes-4]),'header crc'
 assert file_bytes==len(data) and zlib.crc32(data[hdr_bytes:])==pcrc,'payload crc'
 L=meta_layout(n_rec,apps,rs)
 assert meta_off==hdr_bytes and meta_bytes==L['bytes'] and pal_off==meta_off+meta_bytes and btab_off>=pal_off+pal_clen
 assert body_off==btab_off+n_body*BODY.size and body_off+body_bytes==file_bytes
 meta=data[meta_off:meta_off+meta_bytes]
 pal=zlib.decompress(data[pal_off:pal_off+pal_clen]);assert len(pal)==PAL_BYTES and fnv1a(pal)==pal_fnv,'palette'
 bodies=[];pos=0;stored_end=0;by_arena={}
 for i in range(n_body):
  stored,arena,nbytes,clen,h,stride=BODY.unpack_from(data,btab_off+i*BODY.size)
  assert arena==pos and stored==stored_end,'body table order'
  if clen:
   raw=bytearray(zlib.decompress(data[body_off+stored:body_off+stored+clen]));assert len(raw)==nbytes
   if stride:
    assert nbytes%stride==0
    for j in range(stride,nbytes):raw[j]=(raw[j]+raw[j-stride])&255
   stored_end=stored+clen
  else:raw=bytearray(data[body_off+stored:body_off+stored+nbytes]);stored_end=stored+nbytes
  assert not check_fnv or fnv1a(raw)==h,'body hash';bodies.append(bytes(raw));by_arena[arena]=bodies[-1];pos+=nbytes
 assert pos==arena_bytes
 assert l0_n<=n_body and sum(len(b) for b in bodies[:l0_n])==l0_arena
 assert (BODY.unpack_from(data,btab_off+l0_n*BODY.size)[0] if l0_n<n_body else stored_end)==l0_stored,'level-0 section'
 recs=[RECORD.unpack_from(meta,i*24) for i in range(n_rec)]
 for r in recs:
  assert r[4] in by_arena and len(by_arena[r[4]])==r[5]==r[7]*r[8],'record body'
 def u16(off,n):return struct.unpack_from('<%dH'%n,meta,off)
 idx=dict(settings=u16(L['idx_s'],648),icons=u16(L['idx_i'],apps*27),names=u16(L['idx_n'],apps*9))
 for i,v in enumerate(idx['settings']):
  if v!=NONE:assert recs[v][:4]==(0,i//18,(i//9)%2,i%9)
 for i,v in enumerate(idx['icons']):
  if v!=NONE:assert recs[v][:4]==(1,i//27,(i//9)%3,i%9)
 for i,v in enumerate(idx['names']):
  if v!=NONE:assert recs[v][:4]==(2,i//9,0,i%9)
 listed=set(v for part in idx.values() for v in part if v!=NONE);assert listed==set(range(n_rec)),'index coverage'
 curve=meta[L['curve']:L['curve']+CURVE_N*2]
 icons=struct.unpack_from('<%dh'%apps,meta,L['reg_icon']);offs=struct.unpack_from('<%dH'%apps,meta,L['reg_name'])
 strings=meta[L['str']:L['str']+rs]
 names=[strings[o:strings.index(b'\0',o)].decode('ascii') for o in offs]
 return dict(flags=flags,build=build.hex(),blob=blob.hex(),records=recs,bodies=bodies,by_arena=by_arena,pal=pal,curve=curve,
             registry=[[i,icons[i],names[i]] for i in range(apps)],arena_bytes=arena_bytes,l0_bodies=l0_n,l0_arena=l0_arena,
             l0_stored=l0_stored,body_bytes=body_bytes,n_bodies=n_body,file_bytes=file_bytes,crc32=pcrc)

INPUTS=['nos/apps/registry.c','nos/ui.c','nos/ui.h','nos/dot.c','nos/dot_glyphs.c','core/gfx.c','core/sprite.h']
INPUTS += [p.relative_to(REPO).as_posix() for p in sorted((REPO/'nos/fonts').glob('*.[ch]'))]
INPUTS += ['nos/dot.h','tools/native_assets/export_settings.c','tools/native_assets/export_launcher.c']
INPUTS += [p.relative_to(REPO).as_posix() for p in sorted((REPO/'core').glob('*')) if p.suffix in ('.c','.h','.inc') and p.stem.startswith(('gfx','font','sprite','motion'))]
INPUTS += ['nos/apps/native_tile.h','tools/generate_native_menu_assets.py']
INPUTS=sorted(set(INPUTS))
def c_function(text,name):
 import re
 match=re.search(r'^static[^;\n]*\b'+name+r'\([^;]*?\)\s*\{',text,re.M)
 assert match,name
 start=match.start();brace=text.index('{',match.start());depth=0
 token=re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*|[{}]',re.S)
 for part in token.finditer(text,brace):
  t=part.group()
  if t=='{':depth+=1
  elif t=='}':
   depth-=1
   if not depth:return text[start:part.end()]
 raise AssertionError(name)
def fingerprints():
 import re
 result={p:sha((REPO/p).read_bytes()) for p in INPUTS}
 settings=(REPO/'nos/apps/set_settings.c').read_text()
 launcher=(REPO/'nos/apps/sys_launcher.c').read_text()
 for name in ['cap_base','arm_pill','units_of','value_under','value_w','draw_entry','row_start','row_step_job','row_bounds','row_bounds_reset','entry_geometry_get','build_k']:
  result['nos/apps/set_settings.c#'+name]=sha(c_function(settings,name).encode())
 block=settings[settings.index('static const sentry E[]'):settings.index('#define NE')]
 result['nos/apps/set_settings.c#entry-artwork']=sha(block.encode())
 for name in ['UNIT','ROW_W','ROW_MAX_H','ROW_SLICE','ROW_GAP']:
  line=re.search(r'^#define\s+'+name+r'\s+[^\n]+',settings,re.M).group()
  result['nos/apps/set_settings.c#'+name]=sha(line.encode())
 line=re.search(r'^static const [^\n]*\brow_blur_len[^\n]+',settings,re.M).group()
 result['nos/apps/set_settings.c#row_blur_len']=sha(line.encode())
 for name in ['grey565','level_ink','to_index','bake_sharp_step','bake_blur_step']:
  result['nos/apps/sys_launcher.c#'+name]=sha(c_function(launcher,name).encode())
 for name in ['ROW_P','ICON_X','NAME_DX','BAND','NVAR','ISPR','NLEV']:
  line=re.search(r'^#define\s+'+name+r'\s+[^\n]+',launcher,re.M).group()
  result['nos/apps/sys_launcher.c#'+name]=sha(line.encode())
 for name in ['k_var_r','k_blur_len']:
  line=re.search(r'^static const [^\n]*\b'+name+r'[^\n]+',launcher,re.M).group()
  result['nos/apps/sys_launcher.c#'+name]=sha(line.encode())
 return result
def verify(m):
 assert fingerprints()==m['inputs_sha256'],'stale input: re-export reference pixels'
 blob=(ROOT/'native_assets.bin').read_bytes();assert sha(blob)==m['blob_sha256']
 for a in m['assets']:
  raw=(ROOT/'raw'/f"{a['family']}_{a['entry']:02}_{a['layout']}_{a['level']}.raw").read_bytes()
  assert len(raw)==a['bytes']==a['w']*a['h']
  assert blob[a['offset']:a['offset']+a['bytes']]==raw
  assert sha(raw)==a['sha256']
 for p,meta in m['palettes'].items():assert sha((ROOT/p).read_bytes())==meta['sha256']
 print('PASS immutable bundle:',len(m['assets']),'descriptors;',sum(a['bytes'] for a in m['assets']),'reference pixels;',len(blob),'deduplicated bytes; all source/descriptor/palette SHA256 exact')
def id_header(build_hash):
 return '/* Generated by tools/generate_native_menu_assets.py: binds firmware to its SD/embedded menu pack. */\n#define NATIVE_MENU_BUILD_SHA256 "'+build_hash+'"\n'
def verify_packs(m,directory=PACK_DIR):
 """Independent check of the tracked packs against the manifest: needs no build artifacts."""
 assert ID_HEADER.read_text()==id_header(m['build_input_sha256']),'native_menu_assets_id.h is stale'
 want={}
 for name,meta in m['packs'].items():
  data=(directory/meta['file']).read_bytes()
  assert len(data)==meta['bytes'] and sha(data)==meta['sha256'],name+' pack changed'
  p=parse_pack(data,check_fnv=name=='sharp')
  assert p['build']==m['build_input_sha256'] and p['blob']==m['blob_sha256'],name+' pack is bound to other inputs'
  sharp=name=='sharp';assert bool(p['flags']&PACK_FLAG_SHARP)==sharp
  expected=[a for a in m['assets'] if not sharp or a['level']==0]
  assert len(p['records'])==len(expected)==meta['records']
  for r,a in zip(p['records'],expected):
   assert r[:4]==(FAMILIES[a['family']],a['entry'],a['layout'],a['level']) and r[5]==a['bytes'] and r[6]==a['revision']
   assert r[7:]==(a['w'],a['h'],a['x'],a['y']),'geometry'
   assert sha(p['by_arena'][r[4]])==a['sha256'],'pixels differ from the canonical export'
  at=0
  for n in PAL_ORDER:
   size=m['palettes'][n]['bytes'];assert sha(p['pal'][at:at+size])==m['palettes'][n]['sha256'],n;at+=size
  assert sha(p['curve'])==m['launcher_curve_sha256'] and p['registry']==[list(r) for r in m['launcher_registry']]
  want[name]=p
 full,sharp=want['full'],want['sharp']
 assert sharp['bodies']==full['bodies'][:sharp['n_bodies']] and full['l0_bodies']==sharp['n_bodies'],'level-0 section must be a shared prefix'
 return want
def stage():
 import shutil
 m=json.loads(MANIFEST.read_text());STAGE_DIR.mkdir(parents=True,exist_ok=True)
 src=PACK_DIR/PACK_FILES['sharp'];assert sha(src.read_bytes())==m['packs']['sharp']['sha256']
 shutil.copyfile(src,STAGE_DIR/src.name);print('Staged',(STAGE_DIR/src.name).relative_to(REPO).as_posix(),'for tools/sdsync.py (SD path /sdcard/menu/)')
def main():
 if '--verify' in sys.argv:
  m=json.loads(MANIFEST.read_text())
  assert fingerprints()==m['inputs_sha256'],'stale artwork input: regenerate with --export'
  verify_packs(m)
  print('PASS native menu asset fingerprints and packs (sharp %d B, full %d B; every body hash matches the canonical export)'%(m['packs']['sharp']['bytes'],m['packs']['full']['bytes']))
  return
 if '--stage' in sys.argv:stage();return
 if '--reference-verify' in sys.argv:
  verify(json.loads((ROOT/'manifest.json').read_text()));return
 if '--export' not in sys.argv:
  raise SystemExit('Use --export to rebuild references and packs from source, --verify for a normal build check, --stage to copy the sharp pack to build/sd/menu.')
 export()

def export():
 # This private host build deliberately bypasses normal ROM verification: stale
 # generated artwork must not prevent bootstrapping its replacement from source.
 import subprocess,shutil,concurrent.futures,re
 ROOT.mkdir(parents=True,exist_ok=True);(ROOT/'raw').mkdir(exist_ok=True);(ROOT/'obj').mkdir(exist_ok=True)
 cc=sys.argv[sys.argv.index('--cc')+1] if '--cc' in sys.argv else shutil.which('gcc')
 if not cc:raise SystemExit('Host gcc is required to export canonical menu art.')
 flags=['-O2','-std=c99','-DNOS_PSRAM_TRACKING','-DGFX_COST_MODEL','-Icore','-Inos','-Inos/fonts','-Isim']
 core_names=['asset_job','gfx','font','sprite','anim','notif','battery','pedometer','walk','motion_wake','wake_service','knock','ephem']
 sources=list((REPO/'nos').rglob('*.c'))+[REPO/'core'/f'{n}.c' for n in core_names if (REPO/'core'/f'{n}.c').exists()]
 sources+=list((REPO/'core/fonts').glob('*.c'))+list((REPO/'sim').glob('nos_*.c'))
 headers=sorted([* (REPO/'nos').rglob('*.h'),* (REPO/'core').rglob('*.h'),* (REPO/'sim').rglob('*.h'),* (REPO/'nos').rglob('*.inc'),* (REPO/'core').rglob('*.inc'),* (REPO/'sim').rglob('*.inc')])
 deps=sha(b''.join(p.read_bytes() for p in headers+[REPO/'nos/apps/set_settings.c',REPO/'nos/apps/sys_launcher.c']))
 def compile_one(source):
  relative=source.relative_to(REPO).as_posix()
  object_path=ROOT/'obj'/(relative.replace('/','_')+'.o')
  recipe=sha((cc+str(flags)+deps+relative).encode()+source.read_bytes())
  stamp=object_path.with_suffix('.sha256')
  if not object_path.exists() or not stamp.exists() or stamp.read_text()!=recipe:
   extra=['-Dmain=assets_sim_main'] if relative=='sim/nos_sim.c' else []
   result=subprocess.run([cc,*flags,*extra,'-c',str(source),'-o',str(object_path)],cwd=REPO,capture_output=True,text=True)
   if result.returncode:raise RuntimeError(relative+'\n'+result.stdout+result.stderr)
   stamp.write_text(recipe)
  return relative,object_path
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:objects=dict(pool.map(compile_one,sources))
 boot=ROOT/'boot.txt';boot.write_text('open! settings\nwait 1000\n')
 for family in ['settings','launcher']:
  exporter=REPO/'tools/native_assets'/f'export_{family}.c'
  _,object_path=compile_one(exporter)
  excluded='nos/apps/set_settings.c' if family=='settings' else 'nos/apps/sys_launcher.c'
  binary=ROOT/(family+'.exe')
  subprocess.run([cc,str(object_path),*[str(o) for name,o in objects.items() if name!=excluded],'-o',str(binary),'-lgdi32','-luser32','-lwinmm','-lm'],cwd=REPO,check=True)
  result=subprocess.run([str(binary),'--script',str(boot),'--out',str(ROOT/(family+'-frames')),'--no-media','--quiet'],cwd=REPO,capture_output=True,text=True,timeout=120)
  if result.returncode:raise RuntimeError(family+' export failed\n'+result.stdout+result.stderr)
  (ROOT/(family+'.csv')).write_text(result.stdout)
  if family=='launcher':
   lines=[line[len('APP,'):] for line in result.stdout.splitlines() if line.startswith('APP,')]
   assert lines,'No exported registry identities';(ROOT/'registry.csv').write_text('\n'.join(lines)+'\n')
 print('Built canonical reference exporters from fresh privately owned source objects; normal build objects untouched')

 assets=[];blob=bytearray();dedup={}
 for fn in ['settings.csv','launcher.csv']:
  for row in csv.reader((ROOT/fn).read_text().splitlines()):
   if not row or row[0]!='ASSET':continue
   family=row[1];entry,layout,level,w,h,x,y,size,steps=map(int,row[2:])
   raw=(ROOT/'raw'/f'{family}_{entry:02}_{layout}_{level}.raw').read_bytes();assert len(raw)==size==w*h
   digest=sha(raw);identity=(w,h,digest)
   if identity not in dedup:dedup[identity]=len(blob);blob.extend(raw)
   key=sha(struct.pack('<4B4h',FAMILIES[family],entry,layout,level,w,h,x,y)+raw)
   assets.append(dict(family=family,entry=entry,layout=layout,level=level,w=w,h=h,x=x,y=y,bytes=size,offset=dedup[identity],revision=int(key[:8],16),sha256=digest,bake_steps=steps))
 inputs=fingerprints();build_hash=sha(json.dumps(inputs,sort_keys=True).encode())
 palettes={};pal_data={}
 for name in PAL_ORDER:
  data=(ROOT/name).read_bytes();assert len(data)==(512 if name=='launcher_press.lut' else 33280)
  palettes[name]={'bytes':len(data),'sha256':sha(data),'symbol':'native_palette_'+name[:-4]};pal_data[name]=data
 manifest=dict(format='immutable dense grayscale; independent crop pose; content dedup; little-endian RGB565 palettes',assets=assets,blob_bytes=len(blob),blob_sha256=sha(blob),build_input_sha256=build_hash,inputs_sha256=inputs,palettes=palettes,settings_red_keys=[0xD0C4,0x2F3B],target_pixel_equivalence='Host reference exact. Xtensa/FMA bake hashes still required for target byte identity.')
 (ROOT/'native_assets.bin').write_bytes(blob)
 (ROOT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 verify(manifest)
 apps=1+max(a['entry'] for a in assets if a['family']!='settings')
 registry=[]
 for row in csv.reader((ROOT/'registry.csv').read_text().splitlines()):
  if row:registry.append((int(row[0]),int(row[1]),row[2]))
 assert len(registry)==apps
 curve=(ROOT/'launcher_curve.bin').read_bytes();assert len(curve)==CURVE_N*2
 manifest['launcher_registry']=registry;manifest['launcher_curve_sha256']=sha(curve)
 encoded={};packs={};built={}
 for name in ('sharp','full'):
  data=build_pack(manifest,blob,pal_data,curve,name=='sharp',encoded);built[name]=data
  p=parse_pack(data)
  packs[name]=dict(file=PACK_FILES[name],bytes=len(data),sha256=sha(data),crc32=p['crc32'],records=len(p['records']),bodies=p['n_bodies'],
                   level0_bodies=p['l0_bodies'],level0_arena_bytes=p['l0_arena'],level0_stored_bytes=p['l0_stored'],arena_bytes=p['arena_bytes'],
                   stored_bytes=p['body_bytes'],version=PACK_VERSION,sharp_only=name=='sharp')
 manifest['packs']=packs
 print('PACKS:',{k:(v['bytes'],v['records'],v['bodies'],v['arena_bytes']) for k,v in packs.items()})
 if '--check-export' in sys.argv:
  current=json.loads(MANIFEST.read_text())
  for name,data in built.items():assert (PACK_DIR/PACK_FILES[name]).read_bytes()==data,'fresh '+name+' pack differs from tracked pack'
  assert current['blob_sha256']==manifest['blob_sha256'] and current['inputs_sha256']==manifest['inputs_sha256']
  assert current['palettes']==manifest['palettes'] and current['assets']==manifest['assets']
  assert ID_HEADER.read_text()==id_header(build_hash)
  print('PASS clean-source export reproduces both packs, pixels, palettes and artwork identities; production files untouched')
  return
 PACK_DIR.mkdir(parents=True,exist_ok=True)
 for name,data in built.items():(PACK_DIR/PACK_FILES[name]).write_bytes(data)
 ID_HEADER.write_text(id_header(build_hash))
 MANIFEST.write_text(json.dumps(manifest,indent=2)+'\n')
 verify_packs(manifest);stage()
 print('Wrote assets/sd/menu packs, native_menu_assets_id.h and the fingerprint manifest')

if __name__=='__main__':main()
