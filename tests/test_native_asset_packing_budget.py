"""Menu pack format, budget, determinism, corruption rejection and the real C loader.

The tracked packs in assets/sd/menu are parsed by the generator's strict independent
reader and compared with the manifest (per-asset sha256) and, when present, with the
canonical reference export. The production nos/native_menu_assets.c is then compiled
on the host and must publish exactly the same records, palettes and curve, and reject
every damaged copy. The ESP asset-worker/tinfl path needs target checks.
Compiler temporaries stay in the private A: check tree.
"""
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import generate_native_menu_assets as gen

OUT=ROOT/'build/alpha05/checks/asset-size/tests'
SHARP_FILE_BUDGET=100000   # level-0 only pack as stored on the SD card
SHARP_STORED_BUDGET=80000  # compressed level-0 bodies (one contiguous section)
ARENA_BUDGET=1500000       # decoded bytes allowed in PSRAM
ORIGINAL_STORAGE_BYTES=4479348+161022
HDR_PAYLOAD_CRC=4+2+2+4*2  # byte offset of payload_crc32 in the header

def sha(data):return hashlib.sha256(data).hexdigest()
def read(name):return (gen.PACK_DIR/gen.PACK_FILES[name]).read_bytes()
def refit(buf):
    """Recompute both CRCs so only the per-layer checks can catch the damage."""
    struct.pack_into('<I',buf,HDR_PAYLOAD_CRC,zlib.crc32(bytes(buf[gen.PACK_HEADER.size:])))
    struct.pack_into('<I',buf,gen.PACK_HEADER.size-4,zlib.crc32(bytes(buf[:gen.PACK_HEADER.size-4])))
    return bytes(buf)


def zlib_library():
    if os.name!='nt':return ctypes.util.find_library('z')
    git=shutil.which('git')
    candidates=[Path('C:/Program Files/Git/mingw64/bin/zlib1.dll')]
    if git:candidates.extend([Path(git).parent/'zlib1.dll',Path(git).resolve().parents[1]/'mingw64/bin/zlib1.dll'])
    return next((str(c) for c in candidates if c.is_file()),None)


class MenuPackBudget(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True,exist_ok=True)
        cls.m=json.loads(gen.MANIFEST.read_text())
        cls.raw={k:read(k) for k in gen.PACK_FILES}
        cls.pack={k:gen.parse_pack(v,check_fnv=True) for k,v in cls.raw.items()}
        cls.env=os.environ|{key:str(OUT) for key in ('TEMP','TMP','TMPDIR')}

    def test_tracked_packs_match_manifest_and_every_pixel_hash(self):
        want=gen.verify_packs(self.m)
        self.assertEqual(set(want),{'sharp','full'})
        self.assertEqual(len(self.m['assets']),1073);self.assertEqual(len(self.pack['full']['records']),1073)
        for k in self.raw:
            meta=self.m['packs'][k]
            self.assertEqual((meta['bytes'],meta['sha256']),(len(self.raw[k]),sha(self.raw[k])))
            self.assertEqual(self.pack[k]['build'],self.m['build_input_sha256']);self.assertEqual(self.pack[k]['blob'],self.m['blob_sha256'])
        self.assertTrue(self.pack['sharp']['flags']&gen.PACK_FLAG_SHARP);self.assertFalse(self.pack['full']['flags']&gen.PACK_FLAG_SHARP)
        self.assertEqual(gen.ID_HEADER.read_text(),gen.id_header(self.m['build_input_sha256']))

    def test_launcher_registry_curve_and_palettes_are_exact(self):
        for k,p in self.pack.items():
            self.assertEqual(p['registry'],[list(r) for r in self.m['launcher_registry']],k)
            self.assertEqual(sha(p['curve']),self.m['launcher_curve_sha256'])
            at=0
            for name in gen.PAL_ORDER:
                size=self.m['palettes'][name]['bytes']
                self.assertEqual(sha(p['pal'][at:at+size]),self.m['palettes'][name]['sha256'],name);at+=size
            self.assertEqual(at,len(p['pal']))

    def test_matches_canonical_reference_export_when_present(self):
        export=ROOT/'build/native_menu_asset_export'
        if not (export/'native_assets.bin').exists():self.skipTest('no reference export (run tools/generate_native_menu_assets.py --export)')
        blob=(export/'native_assets.bin').read_bytes();self.assertEqual(sha(blob),self.m['blob_sha256'])
        for a in self.m['assets']:
            self.assertEqual(blob[a['offset']:a['offset']+a['bytes']],(export/'raw'/f"{a['family']}_{a['entry']:02}_{a['layout']}_{a['level']}.raw").read_bytes())
        f=self.pack['full']
        for a,r in zip(self.m['assets'],f['records']):self.assertEqual(f['by_arena'][r[4]],blob[a['offset']:a['offset']+a['bytes']])
        for name in gen.PAL_ORDER:self.assertEqual(sha((export/name).read_bytes()),self.m['palettes'][name]['sha256'])

    def test_sharp_is_a_small_contiguous_prefix_of_full(self):
        s,f=self.pack['sharp'],self.pack['full']
        self.assertEqual(s['bodies'],f['bodies'][:s['n_bodies']]);self.assertEqual(f['l0_bodies'],s['n_bodies'])
        self.assertEqual((s['l0_bodies'],s['l0_arena'],s['l0_stored']),(f['l0_bodies'],f['l0_arena'],f['l0_stored']))
        self.assertEqual(s['arena_bytes'],s['l0_arena'])
        self.assertTrue(all(r[3]==0 for r in s['records']));self.assertEqual(len(s['records']),169)
        self.assertEqual(sum(1 for r in f['records'] if r[3]==0),len(s['records']))
        self.assertLessEqual(len(self.raw['sharp']),SHARP_FILE_BUDGET)
        self.assertLessEqual(s['l0_stored'],SHARP_STORED_BUDGET)
        self.assertLessEqual(s['arena_bytes'],ARENA_BUDGET)
        self.assertLess(len(self.raw['sharp']),ORIGINAL_STORAGE_BYTES//40)
        (OUT/'budget.json').write_text(json.dumps({'old_storage_bytes':ORIGINAL_STORAGE_BYTES,'sharp_pack_bytes':len(self.raw['sharp']),
            'sharp_level0_stored_bytes':s['l0_stored'],'sharp_decoded_bytes':s['arena_bytes'],'full_pack_bytes':len(self.raw['full']),
            'full_decoded_bytes':f['arena_bytes'],'arena_cap':ARENA_BUDGET},indent=2)+'\n')

    def test_rebuild_is_byte_deterministic(self):
        f=self.pack['full'];blob=bytearray(f['arena_bytes'])
        for a,r in zip(self.m['assets'],f['records']):
            body=f['by_arena'][r[4]];blob[a['offset']:a['offset']+len(body)]=body
        self.assertEqual(sha(bytes(blob)),self.m['blob_sha256'])
        pal={};at=0
        for name in gen.PAL_ORDER:
            n=self.m['palettes'][name]['bytes'];pal[name]=f['pal'][at:at+n];at+=n
        for k in ('sharp','full'):
            self.assertEqual(gen.build_pack(self.m,bytes(blob),pal,f['curve'],k=='sharp'),self.raw[k],k)

    def test_python_reader_rejects_damage(self):
        data=self.raw['sharp'];hdr=gen.PACK_HEADER.unpack_from(data)
        for what,bad in (('truncated',data[:-1]),('short',data[:20]),('magic',b'XXXX'+data[4:]),
                         ('payload bit',data[:-9]+bytes([data[-9]^1])+data[-8:]),('header bit',data[:60]+bytes([data[60]^1])+data[61:])):
            with self.assertRaises(AssertionError,msg=what):gen.parse_pack(bad)
        body_off=hdr[3+13]  # a body byte flipped under valid CRCs is caught by the per-body hash
        bad=bytearray(data);bad[body_off+5]^=0x10
        with self.assertRaises((AssertionError,zlib.error)):gen.parse_pack(refit(bad))
        gen.parse_pack(data)

    def test_stage_target_matches_tracked_sharp_pack_if_staged(self):
        staged=gen.STAGE_DIR/gen.PACK_FILES['sharp']
        if staged.exists():self.assertEqual(staged.read_bytes(),self.raw['sharp'])

    @unittest.skipUnless(shutil.which('gcc'),'GCC required')
    def test_production_loader_publishes_same_records_and_rejects_damage(self):
        exe=OUT/'native_menu_pack_test.exe'
        command=['gcc','-std=c99','-O1','-Wall','-Wextra','-Werror','-Inos','-Icore','-Inos/fonts','tests/native_menu_pack_test.c',
                 'nos/fonts/nf_body.c','nos/fonts/nf_label.c','nos/fonts/nf_title.c','-o',str(exe)]
        r=subprocess.run(command,cwd=ROOT,env=self.env,capture_output=True,text=True,timeout=120)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        for k in ('sharp','full'):
            p=self.pack[k];reg=OUT/f'registry_{k}.txt'
            reg.write_text(''.join('%d %d %s\n'%tuple(x) for x in p['registry']))
            r=subprocess.run([str(exe),str(gen.PACK_DIR/gen.PACK_FILES[k]),str(reg)],cwd=ROOT,env=self.env,capture_output=True,text=True,timeout=120)
            (OUT/f'loader_{k}.log').write_text(r.stdout+r.stderr);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertIn('PASS native menu pack loader',r.stdout);self.assertIn('NOSASSETS loaded=%d'%len(self.raw[k]),r.stdout)
            self.assertEqual(r.stdout.count('REJECT '),13)
            cache={}
            def fnv(arena):
                if arena not in cache:cache[arena]=gen.fnv1a(p['by_arena'][arena])
                return cache[arena]
            expect={tuple(q[:4]):(q[7],q[8],q[9],q[10],q[5],q[6],fnv(q[4])) for q in p['records']}
            got={}
            for line in r.stdout.splitlines():
                if line.startswith('RECORD '):
                    v=line.split()[1:];got[tuple(map(int,v[:4]))]=(int(v[4]),int(v[5]),int(v[6]),int(v[7]),int(v[8]),int(v[9],16),int(v[10],16))
            self.assertEqual(got,expect,k)
            pal=p['pal'];sz=33280
            want='PAL '+' '.join('%08x'%gen.fnv1a(pal[i*sz:i*sz+(512 if i==4 else sz)]) for i in range(5))
            self.assertIn(want,r.stdout);self.assertIn('CURVE %08x'%gen.fnv1a(p['curve']),r.stdout)

    @unittest.skipUnless(shutil.which('gcc') and os.name=='nt' and zlib_library(),'Windows GCC and a host zlib required')
    def test_esp_sd_branch_with_real_asset_worker_protocol(self):
        # Real loader (ESP branch) + real core/asset_job.c; fake clock, SD mount and tinfl.
        exe=OUT/'native_menu_esp_test.exe';sd=OUT/'esp-sd';(sd/'menu').mkdir(parents=True,exist_ok=True)
        (sd/'menu'/gen.PACK_FILES['sharp']).write_bytes(self.raw['sharp'])
        command=['gcc','-std=c99','-O1','-Wall','-Wextra','-Werror','-DESP_PLATFORM','-Itests/stubs/native_menu_esp','-Inos','-Icore','-Inos/fonts',
                 'tests/native_menu_esp_test.c','core/asset_job.c','nos/fonts/nf_body.c','nos/fonts/nf_label.c','nos/fonts/nf_title.c','-o',str(exe)]
        r=subprocess.run(command,cwd=ROOT,env=self.env,capture_output=True,text=True,timeout=120)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        r=subprocess.run([str(exe),str(sd),zlib_library()],cwd=ROOT,env=self.env,capture_output=True,text=True,timeout=120)
        (OUT/'esp_loader.log').write_text(r.stdout+r.stderr);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('PASS native menu pack ESP loader',r.stdout)
        self.assertEqual(r.stdout.count('NOSASSETS ERROR'),11);self.assertEqual(r.stdout.count('NOSASSETS loaded=%d'%len(self.raw['sharp'])),1)


if __name__=='__main__':unittest.main()
