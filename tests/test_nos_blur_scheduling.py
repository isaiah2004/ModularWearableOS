"""Live animation may miss, but must not starve later static visible/incoming bands."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class BlurSchedulingTests(unittest.TestCase):
    def run_app(self, app):
        with tempfile.TemporaryDirectory(prefix="nos-blur-scheduling-") as tmp:
            exe = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            result = subprocess.run([
                "gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-format-truncation", "-Wno-unused-parameter", "-Icore", "-Inos", "-Inos/apps",
                "-flto", "-fwhole-program", "-ffunction-sections", "-fdata-sections",
                f"-DAPP_CASE={app}", "tests/nos_blur_scheduling_test.c", "core/gfx.c", "core/sprite.c",
                "nos/blur_cache.c", "nos/dot.c", "nos/dot_glyphs.c", "nos/prog.c", "nos/ui.c",
                "nos/motion.c", *map(str, sorted((ROOT / "nos/fonts").glob("*.c"))),
                "-Wl,--gc-sections", "-lm", "-o", str(exe),
            ], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS actual animated ticks", result.stdout)

    def test_weather_static_visible_and_incoming_publish_during_icon_animation(self):
        self.run_app(1)

    def test_about_static_visible_progress_during_glint(self):
        self.run_app(2)


if __name__ == "__main__":
    unittest.main()
