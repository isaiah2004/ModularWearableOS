"""Generic app callers must use the cache selection at slow and fast speeds."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class BlurCallerTests(unittest.TestCase):
    def test_actual_app_ticks_and_authored_live_states(self):
        with tempfile.TemporaryDirectory(prefix="nos-blur-callers-") as tmp:
            for case in range(1, 8):
                with self.subTest(app=case):
                    exe = Path(tmp) / f"app-{case}.exe"
                    result = subprocess.run([
                        "gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-format-truncation", "-flto",
                        "-fwhole-program", "-ffunction-sections", "-fdata-sections",
                        f"-DAPP_CASE={case}", "-Icore", "-Inos", "-Inos/fonts",
                        "tests/nos_blur_callers_test.c", "nos/motion.c", "-Wl,--gc-sections",
                        "-lm", "-o", str(exe),
                    ], cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("uses selected cache shutter", result.stdout)


if __name__ == "__main__":
    unittest.main()
