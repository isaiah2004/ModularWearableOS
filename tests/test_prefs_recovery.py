import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('gcc'),'gcc required')
class PreferenceRecoveryTests(unittest.TestCase):
    def test_legacy_migration_delayed_load_and_wrist_provenance(self):
        with tempfile.TemporaryDirectory(prefix='nos-prefs-recovery-') as directory:
            exe=Path(directory)/('check.exe' if os.name=='nt' else 'check')
            result=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Icore','-Inos',
                'tests/nos_prefs_test.c','nos/prefs.c','-o',str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
