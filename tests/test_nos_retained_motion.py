"""Production retained-cache ownership and bounded moving-work tests."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class RetainedMotionTests(unittest.TestCase):
    def test_exact_ownership_and_fixed_arena_motion(self):
        with tempfile.TemporaryDirectory(prefix="nos-retained-motion-") as directory:
            for name, sources, flags in (
                ("ownership", ["tests/blur_cache_shared_stage_test.c", "nos/blur_cache.c"], []),
                ("motion", ["tests/ui_blur_motion_test.c", "nos/blur_cache.c",
                            "core/gfx.c", "core/sprite.c"], ["-DNOS_PSRAM_TRACKING"]),
            ):
                with self.subTest(name=name):
                    executable = Path(directory) / (name + ".exe")
                    result = subprocess.run(
                        ["gcc", "-O2", "-std=c99", "-Wall", "-Wextra", "-Werror",
                         "-Icore", "-Inos", *flags, *sources, "-lm", "-o", str(executable)],
                        cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(executable)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
