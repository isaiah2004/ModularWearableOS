"""Reference-30Hz physical speeds must select identical cached pixels at any cadence."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class UiBlurCadenceTests(unittest.TestCase):
    def test_ladders_and_warm_raster_at_30_40_60_hz(self):
        with tempfile.TemporaryDirectory(prefix="nos-blur-cadence-") as directory:
            digests = []
            for cadence in (30, 40, 60):
                with self.subTest(cadence=cadence):
                    executable = Path(directory) / f"cadence-{cadence}.exe"
                    compile_result = subprocess.run(
                        ["gcc", "-O2", "-std=c99", "-Wall", "-Wextra", "-Werror",
                         f"-DNOS_FPS={cadence}", "-Icore", "-Inos",
                         "tests/ui_blur_cadence_test.c", "nos/blur_cache.c",
                         "core/gfx.c", "core/sprite.c", "-lm", "-o", str(executable)],
                        cwd=ROOT, capture_output=True, text=True,
                    )
                    self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
                    result = subprocess.run([str(executable)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("PASS cadence-independent", result.stdout)
                    digests.append(re.search(r"digest=([0-9a-f]+)", result.stdout).group(1))
            self.assertEqual(len(set(digests)), 1, digests)


if __name__ == "__main__":
    unittest.main()
