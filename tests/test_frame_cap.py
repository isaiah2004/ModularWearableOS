"""Exercise the production start scheduler and bounded frame trace on the host."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which('gcc'),'gcc required')
class FrameCapTests(unittest.TestCase):
    def test_scheduler_and_trace(self):
        with tempfile.TemporaryDirectory(prefix='frame-cap-',dir=ROOT/'build') as directory:
            for source in ('nos_frame_pacer_test.c','nos_list_profile_test.c'):
                with self.subTest(source=source):
                    exe=Path(directory)/(source+'.exe')
                    compiled=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-Ifirmware/main','tests/'+source,'-o',str(exe)],cwd=ROOT,capture_output=True,text=True)
                    self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
                    run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)
if __name__=='__main__':unittest.main()
