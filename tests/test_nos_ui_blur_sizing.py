import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class BlurSizingTests(unittest.TestCase):
    def check_binary(self, source, extra_sources=(), definitions=(), arguments=((),)):
        with tempfile.TemporaryDirectory(prefix="nos-blur-sizing-") as tmp:
            binary = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            result = subprocess.run(
                ["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Icore", "-Inos", *definitions,
                 source, "nos/blur_cache.c", *extra_sources, "-lm", "-o", str(binary)],
                cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for args in arguments:
                with self.subTest(arguments=args):
                    result = subprocess.run([str(binary), *args], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_demand_reserve_and_published_lifecycle(self):
        self.check_binary("tests/ui_blur_sizing_test.c", ("core/gfx.c", "core/sprite.c"),
                          ("-DNOS_PSRAM_TRACKING",))

    def test_shared_stage_scalar_pixels_fragmentation_and_rollback(self):
        self.check_binary("tests/blur_cache_shared_stage_test.c")

    def test_motion_cold_priority_refinement_reversal_and_thresholds(self):
        self.check_binary("tests/ui_blur_motion_test.c", ("core/gfx.c", "core/sprite.c"),
                          ("-DNOS_PSRAM_TRACKING",))

    def test_rejected_sources_yield_and_recover_with_bounded_probes(self):
        self.check_binary("tests/ui_blur_retry_test.c", ("core/gfx.c", "core/sprite.c"),
                          ("-DNOS_PSRAM_TRACKING",))

    def test_existing_cache_and_allocation_failure(self):
        self.check_binary("tests/ui_blur_cache_test.c",
                          ("nos/ui_blur.c", "core/gfx.c", "core/sprite.c"),
                          arguments=((), ("--allocation-failure",)))

    def test_existing_glyph_and_control_pixels(self):
        self.check_binary("tests/ui_blur_raster_test.c",
                          ("core/gfx.c", "core/sprite.c", "core/font.c", "core/fonts/font_small.c"))

    def test_existing_scalar_filters_and_eviction(self):
        self.check_binary("tests/blur_cache_test.c")


if __name__ == "__main__":
    unittest.main()
