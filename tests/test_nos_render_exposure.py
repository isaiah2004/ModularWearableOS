"""Immutable producer/worker exposure snapshot oracle; no device access."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class RenderExposureTests(unittest.TestCase):
    def test_synchronous_cached_run_partition_matches_original(self):
        # Exercise the current engine function, with only timing and global
        # eligibility replaced by test arguments. Independent original remains.
        engine = (ROOT / "nos/nos.c").read_text()
        start = engine.index("static void apply_exposure(")
        stop = engine.index("static void finish_exposure(", start)
        actual = engine[start:stop].replace("static void apply_exposure(", "static void grouped(")
        actual = actual.replace("int y1)", "int y1, int enabled)")
        actual = actual.replace("nos_temporal_blur && !nos_aod", "enabled")
        actual = actual.replace("ui_fblur_row_spans_run", "spans_run")
        actual = "\n".join(line for line in actual.splitlines()
                           if "hal_micros()" not in line)
        source = (ROOT / "tests/nos_render_exposure_sync_test.c").read_text()
        start = source.index("static void grouped(")
        stop = source.index("static uint32_t rng", start)
        source = source[:start] + actual + "\n" + source[stop:]
        source = source.replace('../nos/render_history.c',
                                (ROOT / 'nos/render_history.c').as_posix())
        with tempfile.TemporaryDirectory(prefix="nos-exposure-sync-") as tmp:
            path = Path(tmp) / "check.c"
            path.write_text(source)
            binary = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     "-Icore", "-Inos", str(path), "nos/render_exposure.c",
                                     "-o", str(binary)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS6000 synchronous", result.stdout)

    def test_snapshot_survives_producer_changes_with_exact_history(self):
        with tempfile.TemporaryDirectory(prefix="nos-exposure-") as tmp:
            binary = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            result = subprocess.run([
                "gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Icore", "-Inos", "tests/nos_render_exposure_test.c",
                "nos/render_exposure.c", "-o", str(binary)],
                cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS6000", result.stdout)


if __name__ == "__main__":
    unittest.main()
