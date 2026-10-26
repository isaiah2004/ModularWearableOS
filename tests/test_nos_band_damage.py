"""Bounded band union: independent coverage, clipping and conservative fallback."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import csv
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from render_private_build import AVAILABLE, CHECKS, environment, simulator
@unittest.skipUnless(shutil.which('gcc'),'gcc required')
class BandDamageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        CHECKS.mkdir(parents=True, exist_ok=True)

    def test_independent_union_and_glass_clipping(self):
        with tempfile.TemporaryDirectory(prefix='band-damage-',dir=CHECKS) as tmp:
            exe=Path(tmp)/'test.exe'
            env=environment(Path(tmp))
            subprocess.run(['gcc','-O2','-std=c99','-Wall','-Wextra','-Werror','-Icore','-Inos','tests/band_damage_test.c','nos/band_damage.c','core/gfx.c','-lm','-o',str(exe)],cwd=ROOT,env=env,check=True,capture_output=True)
            result=subprocess.run([str(exe)],cwd=Path(tmp),env=env,capture_output=True,text=True,check=True)
            self.assertIn('450000 independent',result.stdout)

    @unittest.skipUnless(AVAILABLE, 'Windows and gcc required')
    def test_real_engine_modes_tail_and_replay(self):
        with tempfile.TemporaryDirectory(prefix='band-engine-',dir=CHECKS) as tmp:
            out=Path(tmp)
            exe=out/'candidate.exe'
            simulator(exe)
            body="""skipboot
root settings
wait 1200
slide 233 370 233 100 600 100
slide 233 100 233 370 400 100
root home
wait 500
press
wait 500
drag 233 370 233 100 500
wait 300
expect memfailures 0
"""
            tape=out/'quality.txt';hashes=[]
            for mode in [0,1]:
                script=out/f'mode{mode}.txt';script.write_text(f'set damage_bands {mode}\nexpect damage_bands {mode}\n'+body)
                target=out/f'mode{mode}';target.mkdir();hp=target/'hashes.txt'
                result=subprocess.run([str(exe),'--script',str(script),'--out',str(target),'--hashes',str(hp),'--no-media','--quiet','--pixel-reference','--quality-'+('record' if mode==0 else 'replay'),str(tape)],cwd=out,env=environment(out),capture_output=True,text=True,timeout=90)
                (target/'run.log').write_text(result.stdout+result.stderr)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                hashes.append(hp.read_text().splitlines())
            self.assertGreater(len(hashes[0]),100);self.assertEqual(hashes[0],hashes[1])
            boundary=ROOT/'tests/nos_band_damage_boundary.txt'
            result=subprocess.run([str(exe),'--damagecheck',str(boundary),'--out',str(out/'boundary')],cwd=out,env=environment(out),capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS damagecheck',result.stdout)
            noop=[]
            with (out/'boundary/damage/frames.csv').open() as stream:
                for row in csv.DictReader(stream):
                    if row['section'] in ['noop_on','noop_off']:
                        noop.append(row);self.assertEqual(row['full'],'0',row)
            self.assertGreater(len(noop),0)

if __name__=='__main__':unittest.main()
