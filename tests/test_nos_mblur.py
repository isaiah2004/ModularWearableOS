"""Live blur cadence, admission and streaming-filter recording contract."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class MotionBlurTests(unittest.TestCase):
    def test_primary_fallback_and_switch_preserve_pixels_and_counters(self):
        with tempfile.TemporaryDirectory(prefix="nos-v-modes-") as tmp:
            exe = Path(tmp) / "modes.exe"
            result = subprocess.run([
                "gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-format-truncation", "-flto",
                "-fwhole-program", "-ffunction-sections", "-fdata-sections",
                "-Icore", "-Inos", "-Inos/fonts", "tests/mblur_modes_test.c",
                "nos/mblur.c", "-Wl,--gc-sections", "-lm", "-o", str(exe),
            ], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("actual-area counters PASS", result.stdout)

    def test_full_column_kernel_keeps_detail_and_matches_scalar_rgb(self):
        with tempfile.TemporaryDirectory(prefix="nos-v-columns-") as tmp:
            exe = Path(tmp) / "columns.exe"
            result = subprocess.run([
                "gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Icore", "-Inos", "tests/mblur_columns_test.c", "nos/mblur.c",
                "-o", str(exe),
            ], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("thin strokes PASS", result.stdout)

    def test_cadence_live_calibration_budget_and_record_epoch(self):
        with tempfile.TemporaryDirectory(prefix="nos-mblur-") as tmp:
            exe = Path(tmp) / "gate.exe"
            result = subprocess.run([
                "gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-format-truncation", "-flto",
                "-fwhole-program", "-ffunction-sections", "-fdata-sections",
                "-Icore", "-Inos", "-Inos/fonts", "tests/mblur_gate_test.c",
                "-Wl,--gc-sections", "-lm", "-o", str(exe),
            ], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("record epoch PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
