import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('gcc'), 'gcc is required')
class SharpRenderTests(unittest.TestCase):
    def compile_run(self,name,definitions,sources):
        base=ROOT/'build/sharp-20261022/tests';base.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=name+'-',dir=base) as temporary:
            binary=Path(temporary)/(name+('.exe' if os.name=='nt' else ''))
            args=['gcc','-O2','-std=c11','-Wall','-Wextra','-Werror','-D__USE_MINGW_ANSI_STDIO=1',
                  '-ffunction-sections','-fdata-sections','-Icore','-Inos','-Inos/fonts',*definitions,*sources,'-Wl,--gc-sections','-lm','-o',str(binary)]
            result=subprocess.run(args,cwd=ROOT,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],cwd=ROOT,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS',result.stdout)
    def test_nested_cost_accounting(self):
        self.compile_run('cost',['-DNOS_RENDER_PROFILING=1'],['tests/cost_profile_test.c','nos/cost_profile.c'])
    def test_sharp_pixels_and_history_absence(self):
        self.compile_run('sharp',['-DNOS_RENDER_SHARP=1'],['tests/sharp_native_test.c','nos/render_history.c','core/sprite.c','core/gfx.c'])

if __name__=='__main__':unittest.main()
