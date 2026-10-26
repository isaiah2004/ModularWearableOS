"""Actual Orrery/HOME gestures, linked privately without touching shared objects."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, link, run, workspace


@unittest.skipUnless(
    AVAILABLE, "Windows and gcc required",
)
class OrreryGestureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = workspace("orrery-gestures")
        cls.exe = cls.out / "gestures.exe"
        link("tests/orrery_edge_swipe_test.c", cls.exe, exclude={"nos/faces/face_orrery.c"})

    def test_edge_swipe_ownership(self):
        result = run(self.exe, self.out / "run", "skipboot\nface 8\nhome_now\nwait 500\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS real input engine", result.stdout)
        self.assertRegex(result.stdout, r"PASS Orrery edge swipe regressions: [1-9][0-9]* checks")
        print(result.stdout)


if __name__ == "__main__":
    unittest.main()
