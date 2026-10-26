import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('gcc'),'gcc required')
class ServiceIdentityWeatherTests(unittest.TestCase):
    def test_actual_owners_and_causal_weather(self):
        base=ROOT/'build'/'alpha05'/'checks'/'platform'
        base.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='svc-identities-',dir=base) as directory:
            exe=Path(directory)/('check.exe' if os.name=='nt' else 'check')
            args=['gcc','-std=c11','-O2','-Wall','-Wextra','-Wno-unused-parameter','-Wno-format-truncation',
                '-Werror=implicit-function-declaration','-flto','-fwhole-program','-ffunction-sections','-fdata-sections','-Icore','-Inos','-Inos/fonts',
                'tests/svc_identity_weather_test.c','nos/gb.c','-Wl,--gc-sections','-lm','-o',str(exe)]
            p=subprocess.run(args,cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            p=subprocess.run([str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)

if __name__=='__main__':unittest.main()
