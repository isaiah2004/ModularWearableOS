import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('gcc'),'GCC required')
class BackgroundTests(unittest.TestCase):
    def test_observer_history_and_faults(self):
        out=ROOT/'build/background-jobs-tests'
        out.mkdir(parents=True,exist_ok=True)
        exe=out/('background.exe' if os.name=='nt' else 'background')
        args=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Icore','-Inos',
              'tests/background_test.c','-lm','-o',str(exe)]
        p=subprocess.run(args,cwd=ROOT,capture_output=True,text=True,timeout=60)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        p=subprocess.run([str(exe)],cwd=ROOT,capture_output=True,text=True,timeout=30)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS background',p.stdout)

    def test_real_journal_observer_and_writer(self):
        out=ROOT/'build/background-jobs-tests'
        out.mkdir(parents=True,exist_ok=True)
        exe=out/('journal.exe' if os.name=='nt' else 'journal')
        p=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                          '-Wno-format-truncation','-Icore','-Inos',
                          'tests/background_sports_test.c','-lm','-o',str(exe)],
                         cwd=ROOT,capture_output=True,text=True,timeout=60)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        with tempfile.TemporaryDirectory(prefix='journal-',dir=out) as directory:
            p=subprocess.run([str(exe)],cwd=directory,capture_output=True,text=True,timeout=30)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS background',p.stdout)

if __name__=='__main__':unittest.main()
