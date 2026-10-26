"""Actual HOME navigation pixels and instrumented lifecycle, private builds only."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, environment, link, run, simulator, workspace


@unittest.skipUnless(
    AVAILABLE, "Windows and gcc required",
)
class NavigationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = workspace("navigation")
        cls.lifecycle = cls.out / "lifecycle.exe"
        link("tests/nos_navigation_lifecycle_test.c", cls.lifecycle, exclude={"nos/apps/sys_home.c"})
        # This executable uses the actual HOME and actual face implementations;
        # the lifecycle test's instrumented HOME must never stand in for it.
        cls.production = cls.out / "navigation.exe"
        simulator(cls.production)

    def test_hidden_face_and_consumed_down(self):
        result = run(self.lifecycle, self.out / "lifecycle", "skipboot\nface 1\nroot home\nwait 500\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS navigation lifecycle", result.stdout)

    def test_actual_navigation_semantics_and_damage(self):
        for name in ("nos_navigation", "nos_navigation_edges"):
            with self.subTest(fixture=name):
                result = subprocess.run(
                    [str(self.production), "--damagecheck", str(ROOT / "tools" / (name + ".txt")),
                     "--out", str(self.out / name), "--no-media", "--quiet"],
                    cwd=self.out, env=environment(self.out), capture_output=True, text=True, timeout=90,
                )
                (self.out / (name + ".log")).write_text(result.stdout + result.stderr)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertRegex(result.stdout, r"PASS damagecheck: [1-9][0-9]* frames identical")


if __name__ == "__main__":
    unittest.main()
