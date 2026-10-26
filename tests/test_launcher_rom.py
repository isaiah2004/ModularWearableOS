from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, link, run, workspace

@unittest.skipUnless(AVAILABLE,"Windows and gcc required")
class LauncherRomTests(unittest.TestCase):
    def test_actual_rom_and_mutable_fallback_ownership(self):
        out=workspace("launcher-rom")
        exe=out/"rom.exe"
        link("tests/launcher_rom_test.c",exe,exclude={"nos/apps/sys_launcher.c"})
        result=run(exe,out,"root launcher\nwait 500\n")
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("PASS launcher ROM",result.stdout)
if __name__=="__main__":unittest.main()
