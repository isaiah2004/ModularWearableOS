"""Exact Q16 background cache admission, publication, concurrency and shader oracle."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which("gcc"),"gcc required")
class OrreryShadeQ16Tests(unittest.TestCase):
    def test_exact_colors_and_lifetime_failure_states(self):
        with tempfile.TemporaryDirectory(prefix="nos-q16-") as tmp:
            exe=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            flags=[] if os.name=="nt" else ["-pthread"]
            subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror","-Icore","-Inos",
                            "tests/orrery_shade_q16_test.c",*flags,"-lm","-o",str(exe)],cwd=ROOT,check=True)
            for mode in ("admission","allocfail","boundary","concurrent","exhaustive"):
                result=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,mode+result.stdout+result.stderr)
                self.assertIn("PASS",result.stdout)
if __name__=="__main__":unittest.main()
