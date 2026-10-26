import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('gcc'),'gcc required')
class GbFramingTests(unittest.TestCase):
    def test_real_parser_distinguishes_commands_from_documentation(self):
        with tempfile.TemporaryDirectory(prefix='nos-gb-frame-') as directory:
            exe=Path(directory)/('check.exe' if os.name=='nt' else 'check')
            result=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Inos',
                'tests/gb_frame_test.c','nos/gb.c','-lm','-o',str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
