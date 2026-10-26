"""Launcher real-asset continuity and packed clipping, fresh current-source objects."""
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, link, run, workspace

@unittest.skipUnless(AVAILABLE,"Windows and gcc required")
class LauncherContinuityTests(unittest.TestCase):
    def test_actual_assets_and_independent_scalar_mix(self):
        out=workspace("launcher-continuity")
        binary=out/"check.exe"
        link("tests/launcher_continuity_test.c",binary,exclude={"nos/apps/sys_launcher.c"})
        result=run(binary,out,"root launcher\nwait 5000\n")
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("PASS launcher continuity",result.stdout)
if __name__=="__main__":unittest.main()
