"""SHARP Settings must accept the SD sharp pack (level 0 only) without any pixel allocation."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, link, run, workspace


@unittest.skipUnless(AVAILABLE, "Windows and gcc required")
class SettingsSharpPackTests(unittest.TestCase):
    def test_level0_pack_is_complete_for_settings(self):
        out = workspace("settings-sharp-pack")
        exe = out / "check.exe"
        link("tests/settings_sharp_pack_test.c", exe, exclude={"nos/apps/set_settings.c"}, defines=("NOS_RENDER_SHARP=1",))
        pack = ROOT / "assets/sd/menu/native_menu_sharp.pack"
        result = run(exe, out, "skipboot\nroot home\nwait 2\n", timeout=180, env={"NOS_MENU_PACK": str(pack)})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS Settings SHARP pack", result.stdout)


if __name__ == "__main__":
    unittest.main()
