import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('gcc'), 'gcc required')
class HostRequestTests(unittest.TestCase):
    def test_bounded_correlated_envelopes_and_transfer_classification(self):
        base=ROOT/'build'/'alpha05'/'checks'/'platform'
        base.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='host-request-',dir=base) as directory:
            exe=Path(directory)/('check.exe' if os.name=='nt' else 'check')
            result=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Inos',
                'tests/host_request_test.c','nos/host_request.c','-o',str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__': unittest.main()
